#include "stdafx.h"
#include "PSXDisc.h"

#include "Utilities/StrUtil.h"
#include "Utilities/mutex.h"

#include <algorithm>
#include <cstring>
#include <mutex>

LOG_CHANNEL(psx_log, "PSX");

namespace psx
{
	msf lba_to_msf(u32 lba)
	{
		const u32 total = lba + lead_in_sectors;
		return {static_cast<u8>(total / (60 * 75)), static_cast<u8>((total / 75) % 60), static_cast<u8>(total % 75)};
	}

	u32 msf_to_lba(msf value)
	{
		const u32 total = (value.minute * 60u + value.second) * 75u + value.frame;
		return total < lead_in_sectors ? 0 : total - lead_in_sectors;
	}

	namespace
	{
		// "MM:SS:FF" as written in a cue sheet, in sectors. Cue timestamps are
		// relative to the start of the file, so they carry no lead-in.
		bool parse_timestamp(std::string_view text, u32& out_sectors)
		{
			u32 m = 0, s = 0, f = 0;

			if (std::sscanf(std::string(text).c_str(), "%u:%u:%u", &m, &s, &f) != 3)
			{
				return false;
			}

			if (s >= 60 || f >= 75)
			{
				return false;
			}

			out_sectors = (m * 60 + s) * 75 + f;
			return true;
		}

		bool parse_track_mode(std::string_view mode, track_type& out_type, u32& out_sector_size)
		{
			// The forms that actually turn up in PS1 rips. MODE1/2048 is a
			// cooked image (a renamed .iso); everything else is raw.
			if (mode == "AUDIO")
			{
				out_type = track_type::audio;
				out_sector_size = raw_sector_size;
				return true;
			}
			if (mode == "MODE1/2352")
			{
				out_type = track_type::mode1;
				out_sector_size = raw_sector_size;
				return true;
			}
			if (mode == "MODE1/2048")
			{
				out_type = track_type::mode1;
				out_sector_size = 2048;
				return true;
			}
			if (mode == "MODE2/2352")
			{
				out_type = track_type::mode2;
				out_sector_size = raw_sector_size;
				return true;
			}
			if (mode == "MODE2/2336")
			{
				out_type = track_type::mode2;
				out_sector_size = 2336;
				return true;
			}

			return false;
		}

		// Splits on whitespace, honouring the double quotes a cue sheet puts
		// around file names.
		std::vector<std::string> tokenize(const std::string& line)
		{
			std::vector<std::string> out;
			std::string current;
			bool in_quotes = false;

			for (const char c : line)
			{
				if (c == '"')
				{
					in_quotes = !in_quotes;
					continue;
				}

				if (!in_quotes && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
				{
					if (!current.empty())
					{
						out.push_back(std::move(current));
						current.clear();
					}
					continue;
				}

				current += c;
			}

			if (!current.empty())
			{
				out.push_back(std::move(current));
			}

			return out;
		}

		std::string directory_of(const std::string& path)
		{
			const usz pos = path.find_last_of("/\\");
			return pos == umax ? std::string{} : path.substr(0, pos + 1);
		}

		// Cue sheets are routinely written on a case-insensitive filesystem and
		// then shipped to one that is not, so a literal open is not enough.
		fs::file open_relaxed(const std::string& dir, const std::string& name, std::string& resolved)
		{
			resolved = dir + name;

			if (fs::file f{resolved}; f)
			{
				return f;
			}

			for (const auto& entry : fs::dir(dir.empty() ? "." : dir))
			{
				if (entry.is_directory)
				{
					continue;
				}

				if (fmt::to_upper(entry.name) == fmt::to_upper(name))
				{
					resolved = dir + entry.name;

					if (fs::file f{resolved}; f)
					{
						return f;
					}
				}
			}

			return {};
		}
	}

	bool disc::open(const std::string& path)
	{
		m_files.clear();
		m_tracks.clear();
		m_total_sectors = 0;
		m_path = path;
		m_cache.clear();
		m_cache_file = umax;
		m_cache_first = 0;
		m_cache_count = 0;

		std::string ext = path.substr(path.find_last_of('.') + 1);
		ext = fmt::to_upper(ext);

		const bool ok = ext == "CUE" ? parse_cue(path) : open_single_file(path);

		if (!ok || m_tracks.empty())
		{
			m_files.clear();
			m_tracks.clear();
			return false;
		}

		finalize();
		load_subchannel_patches();
		return true;
	}

	// Mirrors webMAN MOD's detect_cd_sector_size(): PSX rips turn up with a
	// range of sector sizes and none of them are declared anywhere, so the
	// only reliable answer is to look for the volume descriptor at the offset
	// each candidate size implies. Sector 16 holds it, hence the <<4.
	u32 detect_sector_size(fs::file& file)
	{
		static constexpr u32 candidates[] = {2352, 2048, 2336, 2448, 2328, 2340, 2368};

		// Sector 16 at the largest candidate size, plus one whole sector.
		std::vector<u8> head(usz{2448} * 17 + 2448);

		if (file.read_at(0, head.data(), head.size()) < head.size())
		{
			return 0;
		}

		for (const u32 size : candidates)
		{
			const usz base = usz{size} << 4;

			if (base + 0x2C > head.size())
			{
				continue;
			}

			const char* at = reinterpret_cast<const char*>(head.data() + base);

			if (std::memcmp(at + 0x20, "PLAYSTATION ", 12) == 0)
			{
				return size;
			}

			if (std::memcmp(at + 0x19, "CD001", 5) == 0 && at[0x18] == 0x01)
			{
				return size;
			}
		}

		return 0;
	}

	bool disc::open_single_file(const std::string& path)
	{
		fs::file file{path};

		if (!file)
		{
			psx_log.error("Failed to open disc image '%s' (%s)", path, fs::g_tls_error);
			return false;
		}

		const u64 size = file.size();

		track t{};
		t.number = 1;
		t.file_index = 0;
		t.file_offset = 0;

		// Prefer what the volume descriptor says; only fall back to guessing
		// from the file size when the image has no recognizable descriptor.
		u32 sector_size = detect_sector_size(file);

		if (sector_size == 0 || size % sector_size != 0)
		{
			if (size % raw_sector_size == 0)
			{
				sector_size = raw_sector_size;
			}
			else if (size % 2048 == 0)
			{
				sector_size = 2048;
			}
			else
			{
				psx_log.error("Disc image '%s' has a size (%d) that matches no known sector size", path, size);
				return false;
			}
		}

		t.sector_size = sector_size;
		t.type = sector_size == 2048 ? track_type::mode1 : track_type::mode2;
		t.length = ::narrow<u32>(size / sector_size);

		psx_log.notice("Disc image '%s': sector size %d", path, sector_size);

		m_files.push_back(std::move(file));
		m_tracks.push_back(t);
		return true;
	}

	bool disc::parse_cue(const std::string& cue_path)
	{
		fs::file cue{cue_path};

		if (!cue)
		{
			psx_log.error("Failed to open cue sheet '%s' (%s)", cue_path, fs::g_tls_error);
			return false;
		}

		const std::string text = cue.to_string();
		const std::string dir = directory_of(cue_path);

		usz current_file = umax;
		bool have_track = false;
		track current{};

		// INDEX 00 is the pregap, INDEX 01 the track proper. Only the delta
		// between them matters, and only when both live in the file.
		u32 pending_index0 = umax;

		const auto flush_track = [&]()
		{
			if (!have_track)
			{
				return;
			}

			if (pending_index0 != umax && current.file_offset / current.sector_size > pending_index0)
			{
				current.pregap += ::narrow<u32>(current.file_offset / current.sector_size - pending_index0);
			}

			m_tracks.push_back(current);
			have_track = false;
			pending_index0 = umax;
		};

		for (const std::string& raw_line : fmt::split(text, {"\n"}))
		{
			const std::vector<std::string> tokens = tokenize(raw_line);

			if (tokens.empty())
			{
				continue;
			}

			const std::string command = fmt::to_upper(tokens[0]);

			if (command == "FILE")
			{
				if (tokens.size() < 2)
				{
					psx_log.error("Cue sheet '%s': FILE without a name", cue_path);
					return false;
				}

				flush_track();

				std::string resolved;
				fs::file file = open_relaxed(dir, tokens[1], resolved);

				if (!file)
				{
					psx_log.error("Cue sheet '%s' references '%s', which could not be opened (%s)", cue_path, resolved, fs::g_tls_error);
					return false;
				}

				m_files.push_back(std::move(file));
				current_file = m_files.size() - 1;
				continue;
			}

			if (command == "TRACK")
			{
				if (current_file == umax)
				{
					psx_log.error("Cue sheet '%s': TRACK before any FILE", cue_path);
					return false;
				}

				if (tokens.size() < 3)
				{
					psx_log.error("Cue sheet '%s': malformed TRACK line", cue_path);
					return false;
				}

				flush_track();

				current = {};
				current.number = static_cast<u32>(std::atoi(tokens[1].c_str()));
				current.file_index = current_file;

				if (!parse_track_mode(fmt::to_upper(tokens[2]), current.type, current.sector_size))
				{
					psx_log.error("Cue sheet '%s': unsupported track mode '%s'", cue_path, tokens[2]);
					return false;
				}

				have_track = true;
				continue;
			}

			if (command == "PREGAP" && have_track)
			{
				u32 sectors = 0;

				if (tokens.size() >= 2 && parse_timestamp(tokens[1], sectors))
				{
					// A PREGAP command describes a gap that is *not* in the
					// file, so it only shifts subsequent addresses.
					current.pregap += sectors;
				}

				continue;
			}

			if (command == "INDEX" && have_track)
			{
				if (tokens.size() < 3)
				{
					continue;
				}

				u32 sectors = 0;

				if (!parse_timestamp(tokens[2], sectors))
				{
					psx_log.error("Cue sheet '%s': malformed INDEX timestamp '%s'", cue_path, tokens[2]);
					return false;
				}

				const int index = std::atoi(tokens[1].c_str());

				if (index == 0)
				{
					pending_index0 = sectors;
				}
				else if (index == 1)
				{
					current.file_offset = u64{sectors} * current.sector_size;
				}

				continue;
			}
		}

		flush_track();

		if (m_tracks.empty())
		{
			psx_log.error("Cue sheet '%s' declares no tracks", cue_path);
			return false;
		}

		return true;
	}

	void disc::finalize()
	{
		// Track lengths are implied: a track runs to the start of the next one
		// in the same file, or to the end of the file.
		u32 lba = 0;

		for (usz i = 0; i < m_tracks.size(); i++)
		{
			track& t = m_tracks[i];

			lba += t.pregap;
			t.start_lba = lba;

			if (t.length == 0)
			{
				u64 end = m_files[t.file_index].size();

				if (i + 1 < m_tracks.size() && m_tracks[i + 1].file_index == t.file_index)
				{
					end = m_tracks[i + 1].file_offset;
				}

				t.length = end > t.file_offset ? ::narrow<u32>((end - t.file_offset) / t.sector_size) : 0;
			}

			lba += t.length;
		}

		m_total_sectors = lba;

		psx_log.notice("Disc '%s': %d track(s), %d sectors", m_path, m_tracks.size(), m_total_sectors);

		for (const track& t : m_tracks)
		{
			const msf start = lba_to_msf(t.start_lba);
			psx_log.notice("  track %02d %s start=%02d:%02d:%02d length=%d", t.number,
				t.is_audio() ? "AUDIO" : "DATA", start.minute, start.second, start.frame, t.length);
		}
	}

	bool is_psx_image(const fs::file& file)
	{
		if (!file)
		{
			return false;
		}

		// The PSX volume descriptor carries "PLAYSTATION " in its system
		// identifier. A PS3 disc image has "CD001" at the same place but not
		// that string, which is what keeps this from swallowing PS3 ISOs.
		static constexpr u32 candidates[] = {2352, 2048, 2336, 2448, 2328, 2340, 2368};

		for (const u32 size : candidates)
		{
			char buf[12]{};

			if (file.read_at((usz{size} << 4) + 0x20, buf, sizeof(buf)) != sizeof(buf))
			{
				continue;
			}

			if (std::memcmp(buf, "PLAYSTATION ", 12) == 0)
			{
				return true;
			}
		}

		return false;
	}

	bool is_psx_image(const std::string& path)
	{
		const usz dot = path.find_last_of('.');
		const std::string ext = dot == umax ? std::string{} : fmt::to_upper(path.substr(dot + 1));

		if (ext == "CUE")
		{
			return true;
		}

		if (ext != "BIN" && ext != "IMG" && ext != "ISO")
		{
			return false;
		}

		return is_psx_image(fs::file{path});
	}

	namespace
	{
		shared_mutex g_mount_mutex;
		std::unique_ptr<disc> g_mounted;
	}

	bool mount(const std::string& path)
	{
		auto next = std::make_unique<disc>();

		if (!next->open(path))
		{
			return false;
		}

		std::lock_guard lock(g_mount_mutex);
		g_mounted = std::move(next);

		psx_log.success("Mounted PSX disc '%s'", path);
		return true;
	}

	void unmount()
	{
		std::lock_guard lock(g_mount_mutex);

		if (g_mounted)
		{
			psx_log.notice("Unmounted PSX disc '%s'", g_mounted->path());
			g_mounted.reset();
		}
	}

	disc* mounted()
	{
		// Reads of the pointer race only against mount/unmount, which happen on
		// boot and shutdown while no guest thread is issuing storage commands.
		return g_mounted.get();
	}

	std::vector<disc::toc_entry> disc::build_toc() const
	{
		std::vector<toc_entry> toc;
		toc.reserve(m_tracks.size() + 1);

		for (const track& t : m_tracks)
		{
			toc_entry entry{};
			entry.adr_control = t.is_audio() ? 0x10 : 0x14;
			entry.track_number = static_cast<u8>(t.number);
			entry.track_start_addr = t.start_lba;
			toc.push_back(entry);
		}

		// MMC requires the lead-out to be reported as track 0xAA. Without it
		// the host has no way to learn where the last track ends, so it cannot
		// tell how long the disc is.
		toc_entry lead_out{};
		lead_out.adr_control = m_tracks.empty() || !m_tracks.back().is_audio() ? 0x14 : 0x10;
		lead_out.track_number = 0xAA;
		lead_out.track_start_addr = m_total_sectors;
		toc.push_back(lead_out);

		return toc;
	}

	void disc::load_subchannel_patches()
	{
		m_subq_patches.clear();
		m_subchannel_file.close();

		const usz dot = m_path.find_last_of('.');

		if (dot == umax)
		{
			return;
		}

		const std::string stem = m_path.substr(0, dot);

		// A full .sub beats both patch formats: it carries every sector's real
		// sub-channel, so there is nothing to reconstruct and no format that
		// can be missing a CRC.
		for (const char* ext : {".sub", ".SUB"})
		{
			if (fs::file file{stem + ext}; file)
			{
				psx_log.success("Using full sub-channel from '%s' (%d sectors)", stem + ext, file.size() / 96);
				m_subchannel_file = std::move(file);
				return;
			}
		}

		// BCD, as written by psxt001z and every tool that followed it.
		const auto from_bcd = [](u8 value) -> u32 { return (value >> 4) * 10 + (value & 0x0F); };

		for (const char* ext : {".lsd", ".LSD", ".sbi", ".SBI"})
		{
			fs::file file{stem + ext};

			if (!file)
			{
				continue;
			}

			const std::vector<u8> blob = file.to_vector<u8>();
			const bool is_sbi = std::string_view(ext).substr(1, 3) == "sbi" || std::string_view(ext).substr(1, 3) == "SBI";

			// SBI opens with the magic "SBI\0"; LSD has no header at all.
			usz pos = 0;

			if (is_sbi)
			{
				if (blob.size() < 4 || std::memcmp(blob.data(), "SBI\0", 4) != 0)
				{
					psx_log.error("'%s' is not a valid SBI file", stem + ext);
					continue;
				}

				pos = 4;
			}

			// SBI: 3 bytes MSF + 1 type byte + 10 Q bytes, no CRC.
			// LSD: 3 bytes MSF + 12 Q bytes, CRC included.
			const usz record = is_sbi ? 14 : 15;

			while (pos + record <= blob.size())
			{
				const u32 absolute = (from_bcd(blob[pos]) * 60 + from_bcd(blob[pos + 1])) * 75 + from_bcd(blob[pos + 2]);
				const u32 lba = absolute < lead_in_sectors ? 0 : absolute - lead_in_sectors;

				std::array<u8, 12> q{};

				if (is_sbi)
				{
					if (blob[pos + 3] != 1)
					{
						psx_log.error("'%s': unexpected record type %d, giving up on it", stem + ext, blob[pos + 3]);
						break;
					}

					std::memcpy(q.data(), blob.data() + pos + 4, 10);

					// An SBI stores no CRC, but libcrypt only checks that the
					// CRC is *wrong* in a particular way. Mednafen reproduces
					// that by flipping the correct one, which is what every
					// tool has copied since; an LSD carries the genuine bytes
					// and needs none of this.
					u16 crc = 0;

					for (u32 i = 0; i < 10; i++)
					{
						crc ^= static_cast<u16>(q[i]) << 8;

						for (u32 bit = 0; bit < 8; bit++)
						{
							crc = (crc & 0x8000) ? static_cast<u16>((crc << 1) ^ 0x1021) : static_cast<u16>(crc << 1);
						}
					}

					crc = static_cast<u16>(~crc ^ 0x0080);
					q[10] = static_cast<u8>(crc >> 8);
					q[11] = static_cast<u8>(crc & 0xFF);
				}
				else
				{
					std::memcpy(q.data(), blob.data() + pos + 3, 12);
				}

				m_subq_patches[lba] = q;
				pos += record;
			}

			psx_log.success("Loaded %d sub-channel patch(es) from '%s'", m_subq_patches.size(), stem + ext);

			if (is_sbi)
			{
				psx_log.warning("'%s' is an SBI, which stores no CRC - the one used here is reconstructed and is wrong for "
					"the minority of libcrypt titles that expect a different variant. A .lsd (redump.org publishes one for "
					"every protected title) or a .sub carries the genuine bytes and always works.", stem + ext);
			}

			return;
		}
	}

	std::string disc::boot_serial()
	{
		// SYSTEM.CNF sits in the root directory, which on every PS1 disc is a
		// handful of sectors past the volume descriptors at lba 16. Scanning a
		// bounded window is enough and avoids walking ISO9660 for one string.
		// Its BOOT line reads: BOOT = cdrom:\SCUS_949.00;1
		constexpr u32 scan_start = 16;
		constexpr u32 scan_end = 512;

		u8 raw[raw_sector_size];

		for (u32 lba = scan_start; lba < std::min(scan_end, m_total_sectors); lba++)
		{
			if (!read_sector(lba, raw))
			{
				break;
			}

			// MODE2 keeps its user data past the 8-byte subheader, MODE1 right
			// after the header.
			const u32 offset = raw[15] == 2 ? 24 : 16;
			const std::string_view data(reinterpret_cast<const char*>(raw + offset), 2048);

			const usz at = data.find("cdrom:");

			if (at == std::string_view::npos)
			{
				continue;
			}

			// Skip "cdrom:" and any leading path separators, then take up to the
			// ';1' version suffix or the end of the token.
			usz begin = at + 6;

			while (begin < data.size() && (data[begin] == '\\' || data[begin] == '/'))
			{
				begin++;
			}

			usz end = begin;

			while (end < data.size() && data[end] != ';' && data[end] > ' ')
			{
				end++;
			}

			if (end > begin)
			{
				std::string serial(data.substr(begin, end - begin));
				psx_log.notice("PSX disc: boot serial '%s' (lba %d)", serial, lba);
				return serial;
			}
		}

		psx_log.warning("PSX disc: no BOOT entry found in SYSTEM.CNF - region will fall back to the default");
		return {};
	}

	void disc::build_q_subchannel(u32 lba, void* out_16) const
	{
		u8* out = static_cast<u8*>(out_16);
		std::memset(out, 0, q_subchannel_size);

		// Real sub-channel data wins outright: its whole point is to differ
		// from what the layout implies.
		if (m_subchannel_file)
		{
			// 96 bytes per sector, Q occupying bytes 12..23.
			if (m_subchannel_file.read_at(u64{lba} * 96 + 12, out, 12) == 12)
			{
				return;
			}
		}

		if (const auto found = m_subq_patches.find(lba); found != m_subq_patches.end())
		{
			std::memcpy(out, found->second.data(), found->second.size());
			return;
		}

		const track* t = track_at(lba);

		if (!t)
		{
			return;
		}

		const auto to_bcd = [](u8 value) -> u8 { return static_cast<u8>(((value / 10) << 4) | (value % 10)); };

		// ADR 1 (position data) in the low nibble; CONTROL in the high one -
		// 4 marks a data track, 0 an audio one.
		out[0] = static_cast<u8>((t->is_audio() ? 0x00 : 0x40) | 0x01);
		out[1] = to_bcd(static_cast<u8>(t->number));
		out[2] = 0x01; // index 1; index 0 would be the pregap

		// Time relative to the start of the track. lba_to_msf is not usable
		// here: it folds in the 150-sector lead-in, which belongs to absolute
		// addresses only - a track-relative time starts at 00:00:00.
		const u32 offset = lba - t->start_lba;
		out[3] = to_bcd(static_cast<u8>(offset / (60 * 75)));
		out[4] = to_bcd(static_cast<u8>((offset / 75) % 60));
		out[5] = to_bcd(static_cast<u8>(offset % 75));
		out[6] = 0;

		const msf absolute = lba_to_msf(lba);
		out[7] = to_bcd(absolute.minute);
		out[8] = to_bcd(absolute.second);
		out[9] = to_bcd(absolute.frame);

		// CRC-16/CCITT over the preceding 10 bytes, stored inverted as the Red
		// Book specifies.
		u16 crc = 0;

		for (u32 i = 0; i < 10; i++)
		{
			crc ^= static_cast<u16>(out[i]) << 8;

			for (u32 bit = 0; bit < 8; bit++)
			{
				crc = (crc & 0x8000) ? static_cast<u16>((crc << 1) ^ 0x1021) : static_cast<u16>(crc << 1);
			}
		}

		crc = ~crc;
		out[10] = static_cast<u8>(crc >> 8);
		out[11] = static_cast<u8>(crc & 0xFF);
	}

	const track* disc::track_at(u32 lba) const
	{
		for (const track& t : m_tracks)
		{
			if (lba >= t.start_lba && lba < t.start_lba + t.length)
			{
				return &t;
			}
		}

		return nullptr;
	}

	bool disc::read_sector(u32 lba, void* out_2352)
	{
		std::memset(out_2352, 0, raw_sector_size);

		const track* t = track_at(lba);

		if (!t)
		{
			return false;
		}

		m_last_lba = lba;

		fs::file& file = m_files[t->file_index];
		const u64 offset = t->file_offset + u64{lba - t->start_lba} * t->sector_size;

		if (t->sector_size == raw_sector_size)
		{
			// Refill the read-ahead window when the sector falls outside it.
			// Tracks are cached separately because each has its own backing
			// file and sector layout.
			if (t->file_index != m_cache_file || lba < m_cache_first || lba >= m_cache_first + m_cache_count)
			{
				m_cache.resize(usz{cache_sectors} * raw_sector_size);

				const u32 available = t->start_lba + t->length - lba;
				const u32 want = std::min(cache_sectors, available);
				const u64 read = file.read_at(offset, m_cache.data(), u64{want} * raw_sector_size);

				m_cache_count = ::narrow<u32>(read / raw_sector_size);
				m_cache_first = lba;
				m_cache_file = t->file_index;

				if (m_cache_count == 0)
				{
					return false;
				}
			}

			std::memcpy(out_2352, m_cache.data() + usz{lba - m_cache_first} * raw_sector_size, raw_sector_size);
			return true;
		}

		// Cooked sector: rebuild the sync pattern and header the drive would
		// have returned. EDC/ECC are left zeroed - the PS1 only validates them
		// on its own error path, and no rip carries them for a cooked image
		// anyway.
		u8* out = static_cast<u8*>(out_2352);

		static constexpr u8 sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
		std::memcpy(out, sync, sizeof(sync));

		const msf address = lba_to_msf(lba);

		// The header stores its address in BCD.
		const auto to_bcd = [](u8 value) -> u8 { return static_cast<u8>(((value / 10) << 4) | (value % 10)); };

		out[12] = to_bcd(address.minute);
		out[13] = to_bcd(address.second);
		out[14] = to_bcd(address.frame);
		out[15] = t->type == track_type::mode2 ? 2 : 1;

		// Both a 2048-byte MODE1 payload and a 2336-byte MODE2 payload start
		// right after the 16-byte sync+header.
		return file.read_at(offset, out + 16, t->sector_size) == t->sector_size;
	}
}
