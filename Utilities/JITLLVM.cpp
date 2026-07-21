#include "util/types.hpp"
#include "util/sysinfo.hpp"
#include "Utilities/Thread.h"
#include "JIT.h"
#include "StrFmt.h"
#include "File.h"
#include "util/logs.hpp"
#include "mutex.h"
#include "util/vm.hpp"
#include "util/asm.hpp"
#include "Crypto/unzip.h"

#include <charconv>

#if defined(__APPLE__)
#include <pthread.h>
#endif

LOG_CHANNEL(jit_log, "JIT");

#ifdef LLVM_AVAILABLE

#include <unordered_map>

#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Weffc++"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#endif
#include <llvm/Support/CodeGen.h>
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolSize.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#ifdef ARCH_ARM64
#include "Emu/CPU/Backends/AArch64/AArch64Common.h"
#endif

namespace
{
	thread_local std::string* g_llvm_fatal_message = nullptr;

	template <typename F>
	bool run_recoverable_llvm(F&& func, std::string& error)
	{
		error.clear();

		// Run LLVM codegen in a disposable thread. If LLVM invokes the fatal
		// handler, only this helper thread exits.
		named_thread worker("LLVM JIT", [&]()
		{
#if defined(__APPLE__)
			pthread_jit_write_protect_np(false);
#endif
			g_llvm_fatal_message = &error;

			std::forward<F>(func)();

			g_llvm_fatal_message = nullptr;
#if defined(__APPLE__)
			pthread_jit_write_protect_np(true);
#endif
		});

		worker();
		const bool result = static_cast<thread_state>(worker) == thread_state::finished;

		if (!result && error.empty())
		{
			error = "LLVM crash recovery invoked";
		}

		return result;
	}
}

const bool jit_initialize = []() -> bool
{
	llvm::InitializeNativeTarget();
	llvm::InitializeNativeTargetAsmPrinter();
	llvm::InitializeNativeTargetAsmParser();
	return true;
}();

[[noreturn]] static void null(const char* name)
{
	fmt::throw_exception("Null function: %s", name);
}

namespace vm
{
	extern u8* const g_sudo_addr;
}

static shared_mutex null_mtx;

static std::unordered_map<std::string, u64> null_funcs;

// Protects jit_compiler::m_global_mapping (all instances)
static shared_mutex s_mutex;

static u64 make_null_function(const std::string& name)
{
	if (name.starts_with("__0x"))
	{
		u32 addr = -1;
		auto res = std::from_chars(name.c_str() + 4, name.c_str() + name.size(), addr, 16);

		if (res.ec == std::errc() && res.ptr == name.c_str() + name.size() && addr < 0x8000'0000)
		{
			fmt::throw_exception("Unhandled symbols cementing! (name='%s'", name);
		}
	}

	std::lock_guard lock(null_mtx);

	if (u64& func_ptr = null_funcs[name]) [[likely]]
	{
		// Already exists
		return func_ptr;
	}
	else
	{
		using namespace asmjit;

		// Build a "null" function that contains its name
		const auto func = build_function_asm<void (*)()>("NULL", [&](native_asm& c, auto& args)
		{
#if defined(ARCH_X64)
			Label data = c.newLabel();
			c.lea(args[0], x86::qword_ptr(data, 0));
			c.jmp(Imm(&null));
			c.align(AlignMode::kCode, 16);
			c.bind(data);

			// Copy function name bytes
			for (char ch : name)
				c.db(ch);
			c.db(0);
			c.align(AlignMode::kData, 16);
#else
			// AArch64 implementation
			Label data = c.newLabel();
			Label jump_address = c.newLabel();
			c.ldr(args[0], arm::ptr(data, 0));
			c.ldr(a64::x14, arm::ptr(jump_address, 0));
			c.br(a64::x14);

			// Data frame
			c.align(AlignMode::kCode, 16);
			c.bind(jump_address);
			c.embedUInt64(reinterpret_cast<u64>(&null));

			c.align(AlignMode::kData, 16);
			c.bind(data);
			c.embed(name.c_str(), name.size());
			c.embedUInt8(0U);
			c.align(AlignMode::kData, 16);
#endif
		});

		func_ptr = reinterpret_cast<u64>(func);
		return func_ptr;
	}
}

struct JITAnnouncer : llvm::JITEventListener
{
	void notifyObjectLoaded(u64, const llvm::object::ObjectFile& obj, const llvm::RuntimeDyld::LoadedObjectInfo& info) override
	{
		using namespace llvm;

		object::OwningBinary<object::ObjectFile> debug_obj_ = info.getObjectForDebug(obj);
		if (!debug_obj_.getBinary())
		{
#ifdef __linux__
			jit_log.error("LLVM: Failed to announce JIT events (no debug object)");
#endif
			return;
		}

		const object::ObjectFile& debug_obj = *debug_obj_.getBinary();

		for (const auto& [sym, size] : computeSymbolSizes(debug_obj))
		{
			Expected<object::SymbolRef::Type> type_ = sym.getType();
			if (!type_ || *type_ != object::SymbolRef::ST_Function)
				continue;

			Expected<StringRef> name = sym.getName();
			if (!name)
				continue;

			Expected<u64> addr = sym.getAddress();
			if (!addr)
				continue;

			jit_announce(*addr, size, {name->data(), name->size()});
		}
	}
};

// Simple memory manager
struct MemoryManager1 : llvm::RTDyldMemoryManager
{
	// 256 MiB for code or data
	static constexpr u64 c_max_size = 0x1000'0000;

	// Allocation unit (2M)
	static constexpr u64 c_page_size = 2 * 1024 * 1024;

	// Reserve 256 MiB blocks
	void* m_code_mems = nullptr;
	void* m_data_ro_mems = nullptr;
	void* m_data_rw_mems = nullptr;

	u64 code_ptr = 0;
	u64 data_ro_ptr = 0;
	u64 data_rw_ptr = 0;

	MemoryManager1() noexcept
	{
		auto ptr = reinterpret_cast<u8*>(utils::memory_reserve(c_max_size * 3, true));
		m_code_mems = ptr;
		// ptr += c_max_size;
		// m_data_ro_mems = ptr;
		 ptr += c_max_size;
		m_data_rw_mems = ptr;
	}

	MemoryManager1(const MemoryManager1&) = delete;

	MemoryManager1& operator=(const MemoryManager1&) = delete;

	~MemoryManager1() override
	{
		// Hack: don't release to prevent reuse of address space, see jit_announce
		// constexpr auto how_much = [](u64 pos) { return utils::align(pos, pos < c_page_size ? c_page_size / 4 : c_page_size); };
		// utils::memory_decommit(m_code_mems, how_much(code_ptr));
		// utils::memory_decommit(m_data_ro_mems, how_much(data_ro_ptr));
		// utils::memory_decommit(m_data_rw_mems, how_much(data_rw_ptr));
		utils::memory_decommit(m_code_mems, c_max_size * 3, true);
	}

	u8* allocate(u64& alloc_pos, void* block, uptr size, u64 align, utils::protection prot)
	{
		align = align ? align : 16;
 
		const u64 sizea = utils::align(size, align);

		if (!size || align > c_page_size || sizea > c_max_size || sizea < size)
		{
			jit_log.fatal("Unsupported size/alignment (size=0x%x, align=0x%x)", size, align);
			return nullptr;
		}

		u64 oldp = alloc_pos;

		u64 olda = utils::align(oldp, align);

		ensure(olda >= oldp);
		ensure(olda < ~sizea);

		u64 newp = olda + sizea;

		if ((newp - 1) / c_max_size != (oldp - 1) / c_max_size)
		{
			constexpr usz num_of_allocations = 1;

			if ((newp - 1) / c_max_size > num_of_allocations)
			{
				// Allocating more than one region does not work for relocations, needs more robust solution
				fmt::throw_exception("Out of memory (size=0x%x, align=0x%x)", size, align);
			}
		}

		// Update allocation counter
		alloc_pos = newp;

		constexpr usz page_quarter = c_page_size / 4;

		// Optimization: split the first allocation to 512 KiB for single-module compilers
		if (oldp < c_page_size && align < page_quarter && (std::min(newp, c_page_size) - 1) / page_quarter != (oldp - 1) / page_quarter)
		{
			const u64 pagea = utils::align(oldp, page_quarter);
			const u64 psize = utils::align(std::min(newp, c_page_size) - pagea, page_quarter);
			utils::memory_commit(reinterpret_cast<u8*>(block) + (pagea % c_max_size), psize, prot);

			// Advance
			oldp = pagea + psize;
		}

		if ((newp - 1) / c_page_size != (oldp - 1) / c_page_size)
		{
			// Allocate pages on demand
			const u64 pagea = utils::align(oldp, c_page_size);
			const u64 psize = utils::align(newp - pagea, c_page_size);
			utils::memory_commit(reinterpret_cast<u8*>(block) + (pagea % c_max_size), psize, prot);
		}

		return reinterpret_cast<u8*>(block) + (olda % c_max_size);
	}

	u8* allocateCodeSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/) override
	{
		return allocate(code_ptr, m_code_mems, size, align, utils::protection::wx);
	}

	u8* allocateDataSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/, bool is_ro) override
	{
		if (is_ro)
		{
			// Disabled
			//return allocate(data_ro_ptr, m_data_ro_mems, size, align, utils::protection::rw);
		}

		return allocate(data_rw_ptr, m_data_rw_mems, size, align, utils::protection::rw);
	}

	bool finalizeMemory(std::string* = nullptr) override
	{
		return false;
	}

	void registerEHFrames(u8*, u64, usz) override
	{
	}

	void deregisterEHFrames() override
	{
	}
};

// Simple memory manager
struct MemoryManager2 : llvm::RTDyldMemoryManager
{
	MemoryManager2() noexcept = default;

	~MemoryManager2() override
	{
	}

	u8* allocateCodeSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/) override
	{
		return jit_runtime::alloc(size, align, true);
	}

	u8* allocateDataSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/, bool /*is_ro*/) override
	{
		return jit_runtime::alloc(size, align, false);
	}

	bool finalizeMemory(std::string* = nullptr) override
	{
		return false;
	}

	void registerEHFrames(u8*, u64, usz) override
	{
	}

	void deregisterEHFrames() override
	{
	}
};

struct ProxyMemoryManager : public llvm::RTDyldMemoryManager
{
	std::shared_ptr<llvm::RTDyldMemoryManager> m_impl;

	ProxyMemoryManager(std::shared_ptr<llvm::RTDyldMemoryManager> impl)
		: m_impl(std::move(impl))
	{
	}

	u8* allocateCodeSection(uptr size, uint align, uint sec_id, llvm::StringRef sec_name) override
	{
		return m_impl->allocateCodeSection(size, align, sec_id, sec_name);
	}

	u8* allocateDataSection(uptr size, uint align, uint sec_id, llvm::StringRef sec_name, bool is_ro) override
	{
		return m_impl->allocateDataSection(size, align, sec_id, sec_name, is_ro);
	}

	bool finalizeMemory(std::string* ErrMsg = nullptr) override
	{
		return m_impl->finalizeMemory(ErrMsg);
	}

	void registerEHFrames(u8* Addr, u64 LoadAddr, usz Size) override
	{
		m_impl->registerEHFrames(Addr, LoadAddr, Size);
	}

	void deregisterEHFrames() override
	{
		m_impl->deregisterEHFrames();
	}
};

// Resolves otherwise-undefined symbols via a custom callback (absolute symbols).
// Returning 0 from the callback means "not handled" (the lookup continues with the next generator).
struct jit_symbol_generator final : llvm::orc::DefinitionGenerator
{
	std::function<u64(const std::string&)> resolve;

	jit_symbol_generator(std::function<u64(const std::string&)> func) noexcept
		: resolve(std::move(func))
	{
	}

	llvm::Error tryToGenerate(llvm::orc::LookupState&, llvm::orc::LookupKind, llvm::orc::JITDylib& jd, llvm::orc::JITDylibLookupFlags, const llvm::orc::SymbolLookupSet& set) override
	{
		llvm::orc::SymbolMap symbols;

		for (const auto& [name, flags] : set)
		{
			if (const u64 addr = resolve((*name).str()))
			{
				symbols[name] = {llvm::orc::ExecutorAddr(addr), llvm::JITSymbolFlags::Exported};
			}
		}

		if (symbols.empty())
		{
			return llvm::Error::success();
		}

		return jd.define(llvm::orc::absoluteSymbols(std::move(symbols)));
	}
};

// Helper class
class ObjectCache final : public llvm::ObjectCache
{
	const std::string& m_path;
	const std::add_pointer_t<jit_compiler> m_compiler = nullptr;

public:
	ObjectCache(const std::string& path, jit_compiler* compiler = nullptr)
		: m_path(path)
		, m_compiler(compiler)
	{
	}

	~ObjectCache() override = default;

	void notifyObjectCompiled(const llvm::Module* _module, llvm::MemoryBufferRef obj) override
	{
		std::string name = m_path;

		name.append(_module->getName());
		//fs::file(name, fs::rewrite).write(obj.getBufferStart(), obj.getBufferSize());
		name.append(".gz");

		if (!obj.getBufferSize())
		{
			jit_log.error("LLVM: Nothing to write: %s", name);
			return;
		}

		ensure(m_compiler);

		fs::pending_file module_file;

		if (!module_file.open((name)))
		{
			jit_log.error("LLVM: Failed to create module file: %s (%s)", name, fs::g_tls_error);
			return;
		}

		// Bold assumption about upper limit of space consumption
		const usz max_size = obj.getBufferSize() * 4;

		if (!m_compiler->add_sub_disk_space(0 - max_size))
		{
			jit_log.error("LLVM: Failed to create module file: %s (not enough disk space left)", name);
			return;
		}

		if (!zip(obj.getBufferStart(), obj.getBufferSize(), module_file.file))
		{
			jit_log.error("LLVM: Failed to compress module: %s", std::string(_module->getName()));
			return;
		}

		jit_log.trace("LLVM: Created module: %s", std::string(_module->getName()));

		// Restore space that was overestimated
		ensure(m_compiler->add_sub_disk_space(max_size - module_file.file.size()));
		module_file.commit();
	}

	static std::unique_ptr<llvm::MemoryBuffer> load(const std::string& path)
	{
		if (fs::file cached{path + ".gz", fs::read})
		{
			const std::vector<u8> cached_data = cached.to_vector<u8>();

			if (cached_data.empty()) [[unlikely]]
			{
				return nullptr;
			}

			const std::vector<u8> out = unzip(cached_data);

			if (out.empty())
			{
				jit_log.error("LLVM: Failed to unzip module: '%s'", path);
				return nullptr;
			}

			auto buf = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(out.size());
			std::memcpy(buf->getBufferStart(), out.data(), out.size());
			return buf;
		}

		if (fs::file cached{path, fs::read})
		{
			if (cached.size() == 0) [[unlikely]]
			{
				return nullptr;
			}

			auto buf = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(cached.size());
			cached.read(buf->getBufferStart(), buf->getBufferSize());
			return buf;
		}

		return nullptr;
	}

	std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module* _module) override
	{
		std::string path = m_path;
		path.append(_module->getName().data());

		if (auto buf = load(path))
		{
			jit_log.notice("LLVM: Loaded module: %s", _module->getName().data());
			return buf;
		}

		return nullptr;
	}
};

std::string jit_compiler::cpu(std::string_view _cpu)
{
	std::string m_cpu = std::string(_cpu);

	if (m_cpu.empty())
	{
		m_cpu = llvm::sys::getHostCPUName().str();

		if (m_cpu == "generic")
		{
			// Try to detect a best match based on other criteria
			m_cpu = fallback_cpu_detection();
		}

		if (m_cpu == "sandybridge" ||
			m_cpu == "ivybridge" ||
			m_cpu == "haswell" ||
			m_cpu == "broadwell" ||
			m_cpu == "skylake" ||
			m_cpu == "skylake-avx512" ||
			m_cpu == "cascadelake" ||
			m_cpu == "cooperlake" ||
			m_cpu == "cannonlake" ||
			m_cpu == "icelake" ||
			m_cpu == "icelake-client" ||
			m_cpu == "icelake-server" ||
			m_cpu == "tigerlake" ||
			m_cpu == "rocketlake" ||
			m_cpu == "alderlake" ||
			m_cpu == "raptorlake" ||
			m_cpu == "meteorlake")
		{
			// Downgrade if AVX is not supported by some chips
			if (!utils::has_avx())
			{
				m_cpu = "nehalem";
			}
		}

		if (m_cpu == "skylake-avx512" ||
			m_cpu == "cascadelake" ||
			m_cpu == "cooperlake" ||
			m_cpu == "cannonlake" ||
			m_cpu == "icelake" ||
			m_cpu == "icelake-client" ||
			m_cpu == "icelake-server" ||
			m_cpu == "tigerlake" ||
			m_cpu == "rocketlake")
		{
			// Downgrade if AVX-512 is disabled or not supported
			if (!utils::has_avx512())
			{
				m_cpu = "skylake";
			}
		}

		if (m_cpu == "znver1" && utils::has_clwb())
		{
			// Upgrade
			m_cpu = "znver2";
		}

		if ((m_cpu == "znver3" || m_cpu == "goldmont" || m_cpu == "alderlake" || m_cpu == "raptorlake" || m_cpu == "meteorlake") && utils::has_avx512_icl())
		{
			// Upgrade
			m_cpu = "icelake-client";
		}

		if (m_cpu == "goldmont" && utils::has_avx2())
		{
			// Upgrade
			m_cpu = "alderlake";
		}
	}

	return m_cpu;
}

std::string jit_compiler::triple1()
{
#if defined(_WIN32)
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#elif defined(__APPLE__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif (defined(__ANDROID__) || defined(__APPLE__)) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-android"); // Set environment to android to reserve x18
#elif defined(__ANDROID__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-android");
#else
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#endif
}

std::string jit_compiler::triple2()
{
#if defined(_WIN32) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif defined(_WIN32) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-gnu");
#elif defined(__APPLE__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif (defined(__ANDROID__) || defined(__APPLE__)) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-android"); // Set environment to android to reserve x18
#elif defined(__ANDROID__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-android"); // Set environment to android to reserve x18
#else
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#endif
}

bool jit_compiler::add_sub_disk_space(ssz space)
{
	if (space >= 0)
	{
		ensure(m_disk_space.fetch_add(space) < ~static_cast<usz>(space));
		return true;
	}

	return m_disk_space.fetch_op([sub_size = static_cast<usz>(0 - space)](usz& val)
	{
		if (val >= sub_size)
		{
			val -= sub_size;
			return true;
		}

		return false;
	}).second;
}

jit_compiler::jit_compiler(const std::unordered_map<std::string, u64>& _link, std::string_view _cpu, u32 flags, std::function<u64(const std::string&)> symbols_cement) noexcept
	: m_context(std::make_unique<llvm::orc::ThreadSafeContext>(std::make_unique<llvm::LLVMContext>()))
	, m_cpu(cpu(_cpu))
{
	[[maybe_unused]] static const bool s_install_llvm_error_handler = []()
	{
		llvm::remove_fatal_error_handler();
		llvm::install_fatal_error_handler([](void*, const char* msg, bool)
		{
			const std::string_view out = msg ? msg : "";

			if (g_llvm_fatal_message)
			{
				*g_llvm_fatal_message = out;
				thread_ctrl::silent_exit();
			}

			fmt::throw_exception("LLVM Emergency Exit Invoked: '%s'", out);
		}, nullptr);

		return true;
	}();

	std::string triple_str;
	if (_link.empty() && !(flags & 0x1))
	{
		triple_str = jit_compiler::triple2();
	}
	else
	{
		triple_str = jit_compiler::triple1();
	}

	std::shared_ptr<llvm::RTDyldMemoryManager> mem;

	if (_link.empty() && !(flags & 0x1))
	{
		mem = std::make_shared<MemoryManager2>();
	}
	else
	{
		mem = std::make_shared<MemoryManager1>();
	}

	std::vector<std::string> attributes;

#if defined(ARCH_ARM64)
	if (utils::has_sha3())
		attributes.push_back("+sha3");
	else
		attributes.push_back("-sha3");

	if (utils::has_dotprod())
		attributes.push_back("+dotprod");
	else
		attributes.push_back("-dotprod");

	if (utils::has_i8mm())
		attributes.push_back("+i8mm");
	else
		attributes.push_back("-i8mm");

	if (utils::has_sve())
		attributes.push_back("+sve");
	else
		attributes.push_back("-sve");

	if (utils::has_sve2())
		attributes.push_back("+sve2");
	else
		attributes.push_back("-sve2");
#endif

	llvm::Triple triple(triple_str);
	llvm::orc::JITTargetMachineBuilder JTMB(triple);
	JTMB.setCPU(m_cpu);
	JTMB.setCodeModel(flags & 0x2 ? llvm::CodeModel::Large : llvm::CodeModel::Small);
	JTMB.setRelocationModel(llvm::Reloc::Model::PIC_);
	JTMB.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
	JTMB.addFeatures(attributes);

	// Own a target machine with the same configuration for eager module compilation
	if (auto tm = JTMB.createTargetMachine())
	{
		m_tm = std::move(*tm);
	}
	else
	{
		fmt::throw_exception("LLVM: Failed to create target machine: %s", llvm::toString(tm.takeError()));
	}

	auto Builder = std::make_unique<llvm::orc::LLJITBuilder>();
	Builder->setJITTargetMachineBuilder(std::move(JTMB));

	const bool link_not_empty = !_link.empty();
	Builder->setObjectLinkingLayerCreator(
		[mem, link_not_empty, flags](llvm::orc::ExecutionSession& ES, const llvm::Triple&)
			-> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>
		{
			auto getMemMgr = [mem]() -> std::unique_ptr<llvm::RTDyldMemoryManager>
			{
				return std::make_unique<ProxyMemoryManager>(mem);
			};

			auto Layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(ES, getMemMgr);

			// Match RuntimeDyld symbol flags handling (weak symbols etc.) with ORC expectations
			Layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
			Layer->setAutoClaimResponsibilityForObjectSymbols(true);

			if (link_not_empty || !(flags & 0x1))
			{
				// Returns null if LLVM wasn't built with Intel JIT events support
				if (auto* intel_listener = llvm::JITEventListener::createIntelJITEventListener())
				{
					Layer->registerJITEventListener(*intel_listener);
				}

				Layer->registerJITEventListener(*(new JITAnnouncer));
			}

			return Layer;
		}
	);

	auto JITOrErr = Builder->create();
	if (!JITOrErr)
	{
		fmt::throw_exception("LLVM: Failed to create LLJIT: %s", llvm::toString(JITOrErr.takeError()));
	}

	m_jit = std::move(*JITOrErr);

	auto& jd = m_jit->getMainJITDylib();

	if (!_link.empty())
	{
		llvm::orc::SymbolMap symbols;
		for (auto&& [name, addr] : _link)
		{
			symbols[m_jit->mangleAndIntern(name)] = {
				llvm::orc::ExecutorAddr(addr),
				llvm::JITSymbolFlags::Exported
			};
		}

		if (auto Err = jd.define(llvm::orc::absoluteSymbols(std::move(symbols))))
		{
			fmt::throw_exception("LLVM: Failed to define linkage symbols: %s", llvm::toString(std::move(Err)));
		}
	}

	// Unresolved symbol lookup order (matches former MCJIT/RTDyldMemoryManager behavior):
	// 1. Custom global mappings (update_global_mapping)
	jd.addGenerator(std::make_unique<jit_symbol_generator>([this](const std::string& name) -> u64
	{
		std::lock_guard lock(s_mutex);

		if (auto it = m_global_mapping.find(name); it != m_global_mapping.end())
		{
			return it->second;
		}

		return 0;
	}));

	// 2. Symbols from the process itself (memcpy, compiler-rt builtins, etc.)
	if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(m_jit->getDataLayout().getGlobalPrefix()))
	{
		jd.addGenerator(std::move(*gen));
	}
	else
	{
		jit_log.error("LLVM: Failed to create process symbol generator: %s", llvm::toString(gen.takeError()));
	}

	// 3. Symbols cementing, then guaranteed fallback to a named "null" function
	jd.addGenerator(std::make_unique<jit_symbol_generator>([cement = std::move(symbols_cement)](const std::string& name) -> u64
	{
		if (cement)
		{
			if (const u64 addr = cement(name))
			{
				return addr;
			}
		}

		return make_null_function(name);
	}));

	fs::device_stat stats{};

	if (fs::statfs(fs::get_cache_dir(), stats))
	{
		m_disk_space = stats.avail_free / 4;
	}
}

llvm::LLVMContext& jit_compiler::get_context()
{
	return *m_context->getContext();
}

llvm::TargetMachine* jit_compiler::get_target_machine() const
{
	return m_tm.get();
}

const llvm::DataLayout& jit_compiler::get_data_layout() const
{
	return m_jit->getDataLayout();
}

jit_compiler& jit_compiler::operator=(thread_state s) noexcept
{
	if (s == thread_state::destroying_context)
	{
		// Release resources explicitly
		m_jit.reset();
		m_context.reset();
	}

	return *this;
}

jit_compiler::~jit_compiler() noexcept
{
}

// Compile a module to an in-memory object file with the given target machine (eager codegen, like MCJIT did)
static std::unique_ptr<llvm::MemoryBuffer> compile_object(llvm::TargetMachine* tm, llvm::Module* _module)
{
	llvm::SmallVector<char, 0> obj_data;
	llvm::raw_svector_ostream obj_os(obj_data);

	llvm::legacy::PassManager pm;

	if (tm->addPassesToEmitFile(pm, obj_os, nullptr, llvm::CodeGenFileType::ObjectFile))
	{
		fmt::throw_exception("LLVM: Target does not support object emission");
	}

	pm.run(*_module);

	return std::make_unique<llvm::SmallVectorMemoryBuffer>(std::move(obj_data), _module->getModuleIdentifier(), false);
}

// Get the name of any external function defined in the module (used to force linking in fin())
static std::string get_module_entry_symbol(const llvm::Module& _module)
{
	for (const auto& func : _module.functions())
	{
		if (!func.isDeclaration() && !func.hasLocalLinkage())
		{
			return func.getName().str();
		}
	}

	return {};
}

// Get the name of any global symbol defined in an object file (used to force linking in fin())
static std::string get_object_entry_symbol(const llvm::object::ObjectFile& obj)
{
	for (const auto& sym : obj.symbols())
	{
		auto flags = sym.getFlags();

		if (!flags)
		{
			llvm::consumeError(flags.takeError());
			continue;
		}

		if ((*flags & llvm::object::SymbolRef::SF_Undefined) || !(*flags & llvm::object::SymbolRef::SF_Global))
		{
			continue;
		}

		if (auto name = sym.getName())
		{
			if (!name->empty())
			{
				return name->str();
			}
		}
		else
		{
			llvm::consumeError(name.takeError());
		}
	}

	return {};
}

// Add a compiled object to the JIT (linking is deferred until fin()/lookup)
void jit_compiler::add_object(std::unique_ptr<llvm::MemoryBuffer> obj, std::string sym)
{
	if (!sym.empty())
	{
		m_pending_syms.emplace_back(std::move(sym));
	}

	if (auto Err = m_jit->addObjectFile(std::move(obj)))
	{
		fmt::throw_exception("LLVM: Failed to add object: %s", llvm::toString(std::move(Err)));
	}
}

void jit_compiler::add(std::unique_ptr<llvm::Module> _module, const std::string& path)
{
	ObjectCache cache{path, this};

	// Load the object from the disk cache if possible, otherwise compile and store it
	std::unique_ptr<llvm::MemoryBuffer> obj = cache.getObject(_module.get());

	if (!obj)
	{
		obj = compile_object(m_tm.get(), _module.get());
		cache.notifyObjectCompiled(_module.get(), obj->getMemBufferRef());
	}

	add_object(std::move(obj), get_module_entry_symbol(*_module));
}

bool jit_compiler::try_add(std::unique_ptr<llvm::Module> _module, const std::string& path, std::string& error)
{
	return run_recoverable_llvm([&]()
	{
		add(std::move(_module), path);
	}, error);
}

void jit_compiler::add(std::unique_ptr<llvm::Module> _module)
{
	auto obj = compile_object(m_tm.get(), _module.get());
	add_object(std::move(obj), get_module_entry_symbol(*_module));
}

bool jit_compiler::try_add(std::unique_ptr<llvm::Module> _module, std::string& error)
{
	return run_recoverable_llvm([&]()
	{
		add(std::move(_module));
	}, error);
}

bool jit_compiler::add(const std::string& path)
{
	auto cache = ObjectCache::load(path);

	if (!cache)
	{
		jit_log.error("ObjectCache: Failed to read file. (path='%s', error=%s)", path, fs::g_tls_error);
		return false;
	}

	std::string sym;

	if (auto obj = llvm::object::ObjectFile::createObjectFile(cache->getMemBufferRef()))
	{
		sym = get_object_entry_symbol(**obj);
	}
	else
	{
		jit_log.error("ObjectCache: Adding failed: %s (%s)", path, llvm::toString(obj.takeError()));
		return false;
	}

	if (auto Err = m_jit->addObjectFile(std::move(cache)))
	{
		jit_log.error("ObjectCache: Adding failed: %s (%s)", path, llvm::toString(std::move(Err)));
		return false;
	}

	if (!sym.empty())
	{
		m_pending_syms.emplace_back(std::move(sym));
	}

	jit_log.trace("ObjectCache: Successfully added %s", path);
	return true;
}

bool jit_compiler::check(const std::string& path)
{
	if (auto cache = ObjectCache::load(path))
	{
		if (auto object_file = llvm::object::ObjectFile::createObjectFile(*cache))
		{
			return true;
		}

		if (fs::remove_file(path))
		{
			jit_log.error("ObjectCache: Removed damaged file: %s", path);
		}
	}

	return false;
}

void jit_compiler::update_global_mapping(const std::string& name, u64 addr)
{
	// Consulted by the JIT for otherwise-unresolved symbols (replace semantics, like MCJIT)
	std::lock_guard lock(s_mutex);
	m_global_mapping[name] = addr;
}

void jit_compiler::clear_global_mapping()
{
	std::lock_guard lock(s_mutex);
	m_global_mapping.clear();
}

void jit_compiler::fin()
{
	// Force linking of all pending objects (like MCJIT finalizeObject)
	for (const std::string& name : std::exchange(m_pending_syms, {}))
	{
		if (auto sym = m_jit->lookup(name); !sym)
		{
			fmt::throw_exception("LLVM: Failed to link object of '%s': %s", name, llvm::toString(sym.takeError()));
		}
	}
}

bool jit_compiler::try_fin(std::string& error)
{
	const std::vector<std::string> pending = std::exchange(m_pending_syms, {});

	const bool result = run_recoverable_llvm([&]()
	{
		for (const std::string& name : pending)
		{
			if (auto sym = m_jit->lookup(name); !sym)
			{
				error = llvm::toString(sym.takeError());
				return;
			}
		}
	}, error);

	return result && error.empty();
}

u64 jit_compiler::get(const std::string& name)
{
	if (auto sym = m_jit->lookup(name))
	{
		return sym->getValue();
	}
	else
	{
		jit_log.error("LLVM: Failed to lookup '%s': %s", name, llvm::toString(sym.takeError()));
		return 0;
	}
}

const char * fallback_cpu_detection()
{
#if defined(ARCH_X64)
	// If we got here we either have a very old and outdated CPU or a new CPU that has not been seen by LLVM yet.
	const std::string brand = utils::get_cpu_brand();
	const auto family = utils::get_cpu_family();
	const auto model = utils::get_cpu_model();

	jit_log.error("CPU wasn't identified by LLVM, brand = %s, family = 0x%x, model = 0x%x", brand, family, model);

	if (brand.starts_with("AMD"))
	{
		switch (family)
		{
		case 0x10:
		case 0x12: // Unimplemented in LLVM
			return "amdfam10";
		case 0x15:
			// Bulldozer class, includes piledriver, excavator, steamroller, etc
			return utils::has_avx2() ? "bdver4" : "bdver1";
		case 0x17:
		case 0x18:
			// No major differences between znver1 and znver2, return the lesser
			return "znver1";
		case 0x19:
			// Models 0-Fh are zen3 as are 20h-60h. The rest we can assume are zen4
			return ((model >= 0x20 && model <= 0x60) || model < 0x10) ? "znver3" : "znver4";
		case 0x1a:
			// Only one generation in family 1a so far, zen5, which we do not support yet.
			// Return zen4 as a workaround until the next LLVM upgrade.
			return "znver4";
		default:
			// Safest guesses
			return utils::has_avx512() ? "znver4" :
			       utils::has_avx2()   ? "znver1" :
			       utils::has_avx()    ? "bdver1" :
			                             "nehalem";
		}
	}
	else if (brand.find("Intel") != std::string::npos)
	{
		if (!utils::has_avx())
		{
			return "nehalem";
		}
		if (!utils::has_avx2())
		{
			return "ivybridge";
		}
		if (!utils::has_avx512())
		{
			return "skylake";
		}
		if (utils::has_avx512_icl())
		{
			return "cannonlake";
		}
		return "icelake-client";
	}
	else if (brand.starts_with("VirtualApple"))
	{
		// No AVX. This will change in MacOS 15+, at which point we may revise this.
		return utils::has_avx() ? "haswell" : "nehalem";
	}

#elif defined(ARCH_ARM64)
#ifdef ANDROID
	static std::string s_result = []() -> std::string
	{
		std::string result = aarch64::get_cpu_name();
		if (result.empty())
		{
			return "cortex-a78";
		}

		std::transform(result.begin(), result.end(), result.begin(), ::tolower);
		return result;
	}();

	return s_result.c_str();
#else
	// TODO: Read the data from /proc/cpuinfo. ARM CPU registers are not accessible from usermode.
	// This will be a pain when supporting snapdragon on windows but we'll cross that bridge when we get there.
	// Require at least armv8-2a. Older chips are going to be useless anyway.
	return "cortex-a78";
#endif
#endif

	// Failed to guess, use generic fallback
	return "generic";
}

#endif // LLVM_AVAILABLE
