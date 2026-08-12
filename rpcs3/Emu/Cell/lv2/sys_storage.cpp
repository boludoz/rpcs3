#include "stdafx.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "sys_event.h"
#include "sys_fs.h"
#include "util/shared_ptr.hpp"

#include "sys_storage.h"

#include "Loader/PSXDisc.h"

LOG_CHANNEL(sys_storage);

namespace
{
	// A PSX disc is served through the BD drive, which is where ps1_emu looks
	// for an inserted disc. On hardware Cobra hooks the same device.
	bool is_psx_disc_device(u64 device)
	{
		return (device & 0xFFFFF00FFFFFFFF) == BDVD_DRIVE && psx::mounted() != nullptr;
	}

	struct storage_manager
	{
		// This is probably wrong and should be assigned per fd or something
		atomic_ptr<lv2_event_queue> asyncequeue;
	};
}

error_code sys_storage_open(u64 device, u64 mode, vm::ptr<u32> fd, u64 flags)
{
	sys_storage.todo("sys_storage_open(device=0x%x, mode=0x%x, fd=*0x%x, flags=0x%x)", device, mode, fd, flags);

	if (device == 0)
	{
		return CELL_ENOENT;
	}

	if (!fd)
	{
		return CELL_EFAULT;
	}

	[[maybe_unused]] u64 storage_id = device & 0xFFFFF00FFFFFFFF;
	fs::file file;

	if (const u32 id = idm::make<lv2_storage>(device, std::move(file), mode, flags))
	{
		*fd = id;
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code sys_storage_close(u32 fd)
{
	sys_storage.todo("sys_storage_close(fd=0x%x)", fd);

	idm::remove<lv2_storage>(fd);

	return CELL_OK;
}

error_code sys_storage_read(u32 fd, u32 mode, u32 start_sector, u32 num_sectors, vm::ptr<void> bounce_buf, vm::ptr<u32> sectors_read, u64 flags)
{
	sys_storage.todo("sys_storage_read(fd=0x%x, mode=0x%x, start_sector=0x%x, num_sectors=0x%x, bounce_buf=*0x%x, sectors_read=*0x%x, flags=0x%x)", fd, mode, start_sector, num_sectors, bounce_buf, sectors_read, flags);

	if (!bounce_buf || !sectors_read)
	{
		return CELL_EFAULT;
	}

	const auto handle = idm::get_unlocked<lv2_storage>(fd);

	if (!handle)
	{
		// Zeroing before the lookup would have to assume a sector size; do it
		// per device below instead, now that the device is known.
		std::memset(bounce_buf.get_ptr(), 0, num_sectors * 0x200ull);
		return CELL_ESRCH;
	}

	if (is_psx_disc_device(handle->device_id))
	{
		// Sectors here are the raw 2352-byte CD sectors advertised by
		// sys_storage_get_device_info for a mounted PSX disc, so the guest
		// buffer is num_sectors * 2352 and a full sector per index fits
		// exactly. read_sector() zero-fills anything it cannot serve.
		psx::disc* disc = psx::mounted();
		u8* out = static_cast<u8*>(bounce_buf.get_ptr());

		std::memset(out, 0, u64{num_sectors} * psx::raw_sector_size);

		u32 done = 0;

		for (; done < num_sectors; done++)
		{
			if (!disc->read_sector(start_sector + done, out + usz{done} * psx::raw_sector_size))
			{
				break;
			}
		}

		if (done < num_sectors)
		{
			sys_storage.warning("sys_storage_read(PSX): lba=0x%x ran past the lead-out (%d of %d sector(s) served)", start_sector, done, num_sectors);
		}

		*sectors_read = done;
		return CELL_OK;
	}

	std::memset(bounce_buf.get_ptr(), 0, num_sectors * 0x200ull);

	if (handle->file)
	{
		handle->file.seek(start_sector * 0x200ull);
		const u64 size = num_sectors * 0x200ull;
		const u64 result = lv2_file::op_read(handle->file, bounce_buf, size);
		num_sectors = ::narrow<u32>(result / 0x200ull);
	}

	*sectors_read = num_sectors;

	return CELL_OK;
}

error_code sys_storage_write(u32 fd, u32 mode, u32 start_sector, u32 num_sectors, vm::ptr<void> data, vm::ptr<u32> sectors_wrote, u64 flags)
{
	sys_storage.todo("sys_storage_write(fd=0x%x, mode=0x%x, start_sector=0x%x, num_sectors=0x%x, data=*=0x%x, sectors_wrote=*0x%x, flags=0x%llx)", fd, mode, start_sector, num_sectors, data, sectors_wrote, flags);

	if (!sectors_wrote)
	{
		return CELL_EFAULT;
	}

	const auto handle = idm::get_unlocked<lv2_storage>(fd);

	if (!handle)
	{
		return CELL_ESRCH;
	}

	*sectors_wrote = num_sectors;

	return CELL_OK;
}

// Resolves a storage handle to the PSX disc it is serving, or nullptr when the
// handle is not a mounted PSX disc in the optical drive.
static psx::disc* psx_scsi_target(u32 fd)
{
	const auto handle = idm::get_unlocked<lv2_storage>(fd);

	return handle && is_psx_disc_device(handle->device_id) ? psx::mounted() : nullptr;
}

// A command block is a command block whichever syscall carries it, so 604, 616
// and 619 all decode it here instead of each one growing its own partial
// implementation that answers a different subset of the same drive.
static error_code exec_psx_scsi_command(psx::disc* disc, const u8* cdb, u64 inlen, u8* data, u64 outlen)
{
	// Every command block, handled or not. An opcode being recognized says
	// nothing about the sub-fields inside it being honoured - READ TOC alone
	// has a format field, an MSF bit, a starting track and an allocation
	// length, and answering the wrong format looks exactly like answering
	// correctly from the outside.
	if (cdb[0] != 0xBE) // READ CD logs its own decoded form below
	{
		std::string cdb_dump;

		for (u64 i = 0; i < std::min<u64>(inlen, 12); i++)
		{
			fmt::append(cdb_dump, "%s%02x", i ? " " : "", cdb[i]);
		}

		sys_storage.notice("SCSI(PSX): cdb=[%s] outlen=0x%llx", cdb_dump, outlen);
	}

	switch (cdb[0])
	{
	case 0xBB: // SET CD SPEED - nothing is actually spinning
	{
		sys_storage.notice("SCSI(PSX): SET CD SPEED %d KB/s", (cdb[2] << 8) | cdb[3]);
		return CELL_OK;
	}

	case 0xBE: // READ CD
	{
		const u32 lba = (u32{cdb[2]} << 24) | (u32{cdb[3]} << 16) | (u32{cdb[4]} << 8) | cdb[5];
		const u32 count = (u32{cdb[6]} << 16) | (u32{cdb[7]} << 8) | cdb[8];
		const u8 flags = cdb[9];
		const u8 subchannel = cdb[10] & 0x07;

		// Bit 4 selects user data, bits 5-6 the headers, bit 7 the sync
		// pattern and bit 3 the EDC/ECC. ps1_emu asks for 0xF8 - all of it,
		// i.e. the whole 2352-byte sector - which is exactly what the disc
		// hands out; anything less would need the sector taken apart.
		const bool wants_raw = (flags & 0xF8) == 0xF8;
		const u32 sector_bytes = (wants_raw ? psx::raw_sector_size : 2048) +
			(subchannel == 0x02 ? psx::disc::q_subchannel_size : 0);

		if (!data || u64{count} * sector_bytes > outlen)
		{
			sys_storage.error("SCSI(PSX): READ CD lba=0x%x count=%d needs %d bytes but out[%d] was provided", lba, count, count * sector_bytes, outlen);
			return CELL_EFAULT;
		}

		for (u32 i = 0; i < count; i++)
		{
			u8* sector = data + usz{i} * sector_bytes;
			u8 raw[psx::raw_sector_size];

			disc->read_sector(lba + i, raw);

			if (wants_raw)
			{
				std::memcpy(sector, raw, psx::raw_sector_size);
			}
			else
			{
				// Cooked read: user data only. MODE2/Form1 keeps it past the
				// 8-byte subheader, MODE1 right after the header.
				const u32 offset = raw[15] == 2 ? 24 : 16;
				std::memcpy(sector, raw + offset, 2048);
			}

			if (subchannel == 0x02)
			{
				disc->build_q_subchannel(lba + i, sector + (wants_raw ? psx::raw_sector_size : 2048));
			}
		}

		// Logged at notice, with a preview of what the sector actually starts
		// with: a wrong offset or a mis-parsed cue shows up here immediately
		// and is otherwise invisible. A raw MODE2 sector begins with the sync
		// pattern 00 FF FF FF FF FF FF FF FF FF FF 00, then the BCD address
		// and the mode byte.
		std::string preview;

		for (u32 i = 0; i < 20 && data; i++)
		{
			fmt::append(preview, "%s%02x", i ? " " : "", data[i]);
		}

		sys_storage.notice("SCSI(PSX): READ CD lba=%d count=%d flags=0x%02x subq=%d -> [%s]", lba, count, flags, subchannel, preview);
		return CELL_OK;
	}

	case 0x43: // READ TOC/PMA/ATIP
	{
		// Byte 1 bit 1 asks for MSF addresses instead of LBA, byte 6 is the
		// track to start from, bytes 7-8 the allocation length.
		const bool msf = (cdb[1] & 0x02) != 0;
		const u8 start_track = cdb[6];
		const u64 allocation = std::min<u64>((u32{cdb[7]} << 8) | cdb[8], outlen);

		std::vector<psx::disc::toc_entry> toc = disc->build_toc();

		// The lead-out (0xAA) is always reported, whatever the start track.
		std::erase_if(toc, [&](const psx::disc::toc_entry& e)
		{
			return e.track_number != 0xAA && e.track_number < start_track;
		});

		const u64 needed = 4 + toc.size() * sizeof(psx::disc::toc_entry);
		std::vector<u8> response(needed, 0);

		const u16 toc_length = static_cast<u16>(needed - 2);
		response[0] = static_cast<u8>(toc_length >> 8);
		response[1] = static_cast<u8>(toc_length & 0xFF);
		response[2] = disc->tracks().empty() ? 0 : static_cast<u8>(disc->tracks().front().number);
		response[3] = disc->tracks().empty() ? 0 : static_cast<u8>(disc->tracks().back().number);

		for (usz i = 0; i < toc.size(); i++)
		{
			u8* entry = response.data() + 4 + i * sizeof(psx::disc::toc_entry);
			entry[0] = 0;
			entry[1] = toc[i].adr_control;
			entry[2] = toc[i].track_number;
			entry[3] = 0;

			const u32 lba = toc[i].track_start_addr;

			if (msf)
			{
				// MSF addresses are 0, M, S, F - and unlike the Q sub-channel
				// they are not BCD here.
				const psx::msf address = psx::lba_to_msf(lba);
				entry[4] = 0;
				entry[5] = address.minute;
				entry[6] = address.second;
				entry[7] = address.frame;
			}
			else
			{
				entry[4] = static_cast<u8>(lba >> 24);
				entry[5] = static_cast<u8>(lba >> 16);
				entry[6] = static_cast<u8>(lba >> 8);
				entry[7] = static_cast<u8>(lba);
			}
		}

		if (!data)
		{
			return CELL_EFAULT;
		}

		std::memcpy(data, response.data(), std::min<u64>(allocation, needed));

		sys_storage.notice("SCSI(PSX): READ TOC msf=%d start=%d answered %d entries", msf, start_track, toc.size());
		return CELL_OK;
	}

	case 0x00: // TEST UNIT READY - a disc is mounted, so the drive is ready
		return CELL_OK;

	case 0x03: // REQUEST SENSE
	{
		// ps1_emu's CD-ROM layer has this wired up as _ioctl_request_sense and
		// logs the result field by field, so an unanswered one does not fail
		// loudly - it reads whatever was already in the buffer and treats that as
		// the drive's error state. Answer "nothing to report" explicitly.
		if (!data || outlen < 18)
		{
			return CELL_EFAULT;
		}

		u8 sense[18]{};

		sense[0] = 0x70; // current error, fixed format
		sense[2] = 0x00; // sense key: NO SENSE
		sense[7] = 10;   // additional sense length (18 - 8)
		sense[12] = 0x00; // ASC
		sense[13] = 0x00; // ASCQ

		std::memcpy(data, sense, std::min<u64>(outlen, sizeof(sense)));

		sys_storage.notice("SCSI(PSX): REQUEST SENSE - no sense");
		return CELL_OK;
	}

	case 0x1E: // PREVENT/ALLOW MEDIUM REMOVAL - nothing can eject here
		return CELL_OK;

	case 0x1B: // START STOP UNIT
		// cdb[4] bit 1 is LOEJ and bit 0 START, so 0x02 ejects and 0x03 loads.
		// This is what webMAN-MOD drives the tray with; a mounted image has no
		// tray to move, and the disc must stay present either way.
		sys_storage.notice("SCSI(PSX): START STOP UNIT 0x%02x - no tray to move", cdb[4]);
		return CELL_OK;

	case 0x2B: // SEEK(10) - reads are absolute, so there is nothing to move
		return CELL_OK;

	case 0x25: // READ CAPACITY
	{
		if (!data || outlen < 8)
		{
			return CELL_EFAULT;
		}

		// Last addressable block, then block size. The lead-out sits one past
		// the last readable sector.
		const u32 last_lba = disc->total_sectors() ? disc->total_sectors() - 1 : 0;

		data[0] = static_cast<u8>(last_lba >> 24);
		data[1] = static_cast<u8>(last_lba >> 16);
		data[2] = static_cast<u8>(last_lba >> 8);
		data[3] = static_cast<u8>(last_lba);
		data[4] = 0;
		data[5] = 0;
		data[6] = static_cast<u8>(psx::raw_sector_size >> 8);
		data[7] = static_cast<u8>(psx::raw_sector_size & 0xFF);

		sys_storage.notice("SCSI(PSX): READ CAPACITY last_lba=%d", last_lba);
		return CELL_OK;
	}

	case 0x42: // READ SUB-CHANNEL
	{
		const bool msf = (cdb[1] & 0x02) != 0;
		const u8 format = cdb[3];

		if (!data || outlen < 4)
		{
			return CELL_EFAULT;
		}

		std::memset(data, 0, std::min<u64>(outlen, 16));

		data[0] = 0;
		data[1] = 0x15; // audio status: no audio to report
		data[2] = 0;
		data[3] = 12; // sub-channel data length

		// Format 1 is "current position"; nothing else is meaningful without
		// a real drive behind this.
		if (format == 0x01 && outlen >= 16)
		{
			const u32 lba = disc->last_lba();
			const psx::track* t = disc->track_at(lba);

			data[4] = 0x01; // sub-channel data format code
			data[5] = t && t->is_audio() ? 0x10 : 0x14;
			data[6] = t ? static_cast<u8>(t->number) : 1;
			data[7] = 1; // index

			const u32 relative = t ? lba - t->start_lba : 0;

			const auto write_addr = [&](u8* dst, u32 value, bool relative_time)
			{
				if (msf)
				{
					dst[0] = 0;

					if (relative_time)
					{
						dst[1] = static_cast<u8>(value / (60 * 75));
						dst[2] = static_cast<u8>((value / 75) % 60);
						dst[3] = static_cast<u8>(value % 75);
					}
					else
					{
						const psx::msf address = psx::lba_to_msf(value);
						dst[1] = address.minute;
						dst[2] = address.second;
						dst[3] = address.frame;
					}
				}
				else
				{
					dst[0] = static_cast<u8>(value >> 24);
					dst[1] = static_cast<u8>(value >> 16);
					dst[2] = static_cast<u8>(value >> 8);
					dst[3] = static_cast<u8>(value);
				}
			};

			write_addr(data + 8, lba, false);
			write_addr(data + 12, relative, true);
		}

		sys_storage.trace("SCSI(PSX): READ SUB-CHANNEL format=0x%02x msf=%d", format, msf);
		return CELL_OK;
	}

	case 0x51: // READ DISC INFORMATION
	{
		if (!data || outlen < 4)
		{
			return CELL_EFAULT;
		}

		// Standard response is 34 bytes; ps1_emu asks for the first 16, which
		// is everything that identifies the disc. Values match what Cobra
		// reports for a mounted image - the emulator is known to accept them.
		u8 info[34]{};

		info[0] = 0;
		info[1] = 32; // length of what follows

		// Disc status 10b = finalized, last session 11b = complete, not
		// erasable. A pressed CD is never anything else.
		info[2] = (3 << 2) | 2;

		info[3] = 1; // first track on disc
		info[4] = 1; // number of sessions (LSB)
		info[5] = 1; // first track of last session (LSB)
		info[6] = 1; // last track of last session (LSB)
		info[7] = 0x20; // URU: the disc is not restricted
		info[8] = 0; // disc type 0x00 = CD-DA or CD-ROM

		// No lead-in/lead-out addresses to report for a finalized disc.
		std::memset(info + 16, 0xFF, 8);

		std::memcpy(data, info, std::min<u64>(outlen, sizeof(info)));

		sys_storage.notice("SCSI(PSX): READ DISC INFORMATION - finalized CD-ROM, %d track(s)", info[6]);
		return CELL_OK;
	}

	case 0x4A: // GET EVENT STATUS NOTIFICATION
	{
		// Byte 4 is the requested notification class bitmap; only the media
		// class (0x10) is meaningful for a disc drive.
		if (cdb[4] != 0x10)
		{
			sys_storage.notice("SCSI(PSX): GET EVENT STATUS for class 0x%02x - ignored", cdb[4]);
			return CELL_OK;
		}

		if (!data || outlen < 8)
		{
			return CELL_EFAULT;
		}

		// 8-byte media event response: a 4-byte header followed by event code,
		// media status and the slot range. The length field counts only what
		// follows the header, so 4 - reporting 6 (bytes after the length field
		// itself) is the other reading of the spec, and it is not the one the
		// drive uses.
		std::memset(data, 0, 8);

		data[0] = 0;
		data[1] = 4;    // event data length
		data[2] = 0x04; // no event pending, media class
		data[3] = 0x0F; // supported classes
		data[4] = 0;    // event code: no change
		data[5] = 0x02; // media present, tray closed

		sys_storage.notice("SCSI(PSX): GET EVENT STATUS - media present");
		return CELL_OK;
	}

	default:
		break;
	}

	// Everything still unknown gets logged with its CDB so the next one can be
	// implemented from evidence rather than guessed at.
	std::string dump;

	for (u64 i = 0; i < std::min<u64>(inlen, 16); i++)
	{
		fmt::append(dump, "%s%02x", i ? " " : "", cdb[i]);
	}

	sys_storage.todo("SCSI(PSX): unhandled opcode 0x%02x cdb=[%s] outlen=0x%llx", cdb[0], dump, outlen);
	return CELL_OK;
}

error_code sys_storage_send_device_command(u32 dev_handle, u64 cmd, vm::ptr<void> in, u64 inlen, vm::ptr<void> out, u64 outlen)
{
	// The 'in' conversion was missing its type ('in=*0x%'), so every argument
	// after it printed as literal text - which hid inlen/outlen at exactly the
	// moment they were needed.
	sys_storage.todo("sys_storage_send_device_command(dev_handle=0x%x, cmd=0x%llx, in=*0x%x, inlen=0x%llx, out=*0x%x, outlen=0x%llx)", dev_handle, cmd, in, inlen, out, outlen);

	// cmd 1 carries a SCSI command block: `in` is a 56-byte structure whose
	// first bytes are the CDB, and `out` receives the data phase. This is how
	// ps1_emu's _xcdrom_thread drives the drive, and answering it is what lets
	// the emulator get past its startup probe.
	psx::disc* disc = psx_scsi_target(dev_handle);

	if (!disc || cmd != 1 || !in || inlen < 12)
	{
		return CELL_OK;
	}

	return exec_psx_scsi_command(disc, static_cast<const u8*>(in.get_ptr()), inlen, out ? static_cast<u8*>(out.get_ptr()) : nullptr, outlen);
}

error_code sys_storage_async_configure(u32 fd, u32 io_buf, u32 equeue_id, u32 unk)
{
	sys_storage.todo("sys_storage_async_configure(fd=0x%x, io_buf=0x%x, equeue_id=0x%x, unk=*0x%x)", fd, io_buf, equeue_id, unk);

	auto& manager = g_fxo->get<storage_manager>();

	if (auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(equeue_id))
	{
		manager.asyncequeue.store(queue);
	}
	else
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_storage_async_send_device_command(u32 dev_handle, u64 cmd, vm::ptr<void> in, u64 inlen, vm::ptr<void> out, u64 outlen, u64 unk)
{
	sys_storage.todo("sys_storage_async_send_device_command(dev_handle=0x%x, cmd=0x%llx, in=*0x%x, inlen=0x%x, out=*0x%x, outlen=0x%x, unk=0x%x)", dev_handle, cmd, in, inlen, out, outlen, unk);

	// The completion event used to be sent without the command ever running, so
	// a caller on this path got told its read had finished into a buffer nobody
	// had written. Nothing here actually defers, so run it and report after.
	if (psx::disc* disc = psx_scsi_target(dev_handle); disc && cmd == 1 && in && inlen >= 12)
	{
		exec_psx_scsi_command(disc, static_cast<const u8*>(in.get_ptr()), inlen, out ? static_cast<u8*>(out.get_ptr()) : nullptr, outlen);
	}

	auto& manager = g_fxo->get<storage_manager>();

	if (auto q = manager.asyncequeue.load())
	{
		// psdevwiki gives 619 the same prototype as 616, ending in a pointer, so
		// `unk` may be a driver_status out-param rather than the user_data tag
		// libfatfs calls it. Echoed into all three words until something observed
		// says which.
		q->send(0, unk, unk, unk);
	}

	return CELL_OK;
}

error_code sys_storage_async_read()
{
	sys_storage.todo("sys_storage_async_read()");

	return CELL_OK;
}

error_code sys_storage_async_write()
{
	sys_storage.todo("sys_storage_async_write()");

	return CELL_OK;
}

error_code sys_storage_async_cancel()
{
	sys_storage.todo("sys_storage_async_cancel()");

	return CELL_OK;
}

error_code sys_storage_get_device_info(u64 device, vm::ptr<StorageDeviceInfo> buffer)
{
	sys_storage.todo("sys_storage_get_device_info(device=0x%x, buffer=*0x%x)", device, buffer);

	if (!buffer)
	{
		return CELL_EFAULT;
	}

	memset(buffer.get_ptr(), 0, sizeof(StorageDeviceInfo));

	u64 storage = device & 0xFFFFF00FFFFFFFF;
	u32 dev_num = (device >> 32) & 0xFF;

	if (storage == ATA_HDD) // dev_hdd?
	{
		if (dev_num > 2)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->flags[1] = 1;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;

		// set partition size based on dev_num
		// stole these sizes from kernel dump, unknown if they are 100% correct
		// vsh reports only 2 partitions even though there is 3 sizes
		switch (dev_num)
		{
		case 0:
			buffer->sector_count = 0x2542EAB0; // possibly total size
			break;
		case 1:
			buffer->sector_count = 0x24FAEA98; // which makes this hdd0
			break;
		case 2:
			buffer->sector_count = 0x3FFFF8; // and this one hdd1
			break;
		}
	}
	else if (storage == BDVD_DRIVE) //	dev_bdvd?
	{
		if (dev_num > 0)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());

		if (psx::disc* disc = psx::mounted())
		{
			// A CD in the drive has different geometry from a BD, and the
			// caller sizes its read buffers from what is reported here. Saying
			// 2048 while sys_storage_read hands back raw 2352-byte sectors is
			// what would overrun the guest buffer, so both have to agree - and
			// raw is the unit Cobra serves a PSX disc in, because the emulator
			// needs the subheader and the CD-DA sectors verbatim.
			buffer->sector_count = disc->total_sectors();
			buffer->sector_size = psx::raw_sector_size;
		}
		else
		{
			buffer->sector_count = 0x4D955;
			buffer->sector_size = 0x800;
		}

		buffer->one = 1;
		buffer->flags[1] = 0;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;
	}
	else if (storage == USB_MASS_STORAGE_1(0))
	{
		if (dev_num > 0)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		/*buffer->sector_count = 0x4D955;*/
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->flags[1] = 0;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;
	}
	else if (storage == NAND_FLASH)
	{
		if (dev_num > 6)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->flags[1] = 1;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x80000;
			break;
		case 1: buffer->sector_count = 0x75F8;
			break;
		case 2: buffer->sector_count = 0x63E00;
			break;
		case 3: buffer->sector_count = 0x8000;
			break;
		case 4: buffer->sector_count = 0x400;
			break;
		case 5: buffer->sector_count = 0x2000;
			break;
		case 6: buffer->sector_count = 0x200;
			break;
		}
	}
	else if (storage == NOR_FLASH)
	{
		if (dev_num > 3)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->flags[1] = 0;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x8000;
			break;
		case 1: buffer->sector_count = 0x77F8;
			break;
		case 2: buffer->sector_count = 0x100; // offset, 0x20000
			break;
		case 3: buffer->sector_count = 0x400;
			break;
		}
	}
	else if (storage == NAND_UNK)
	{
		if (dev_num > 1)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x800;
		buffer->one = 1;
		buffer->flags[1] = 0;
		buffer->flags[2] = 1;
		buffer->flags[7] = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x7FFFFFFF;
			break;
		}
	}
	else
	{
		sys_storage.error("sys_storage_get_device_info(device=0x%x, buffer=*0x%x)", device, buffer);
	}

	return CELL_OK;
}

error_code sys_storage_get_device_config(vm::ptr<u32> storages, vm::ptr<u32> devices)
{
	sys_storage.todo("sys_storage_get_device_config(storages=*0x%x, devices=*0x%x)", storages, devices);

	if (storages) *storages = 6; else return CELL_EFAULT;
	if (devices)  *devices = 17; else return CELL_EFAULT;

	return CELL_OK;
}

error_code sys_storage_report_devices(u32 storages, u32 start, u32 devices, vm::ptr<u64> device_ids)
{
	sys_storage.todo("sys_storage_report_devices(storages=0x%x, start=0x%x, devices=0x%x, device_ids=0x%x)", storages, start, devices, device_ids);

	if (!device_ids)
	{
		return CELL_EFAULT;
	}

	static constexpr std::array<u64, 0x11> all_devs = []
	{
		std::array<u64, 0x11> all_devs{};
		all_devs[0] = 0x10300000000000A;

		for (int i = 0; i < 7; ++i)
		{
			all_devs[i + 1] = 0x100000000000001 | (static_cast<u64>(i) << 32);
		}

		for (int i = 0; i < 3; ++i)
		{
			all_devs[i + 8] = 0x101000000000007 | (static_cast<u64>(i) << 32);
		}

		all_devs[11] = 0x101000000000006;

		for (int i = 0; i < 4; ++i)
		{
			all_devs[i + 12] = 0x100000000000004 | (static_cast<u64>(i) << 32);
		}

		all_devs[16] = 0x100000000000003;
		return all_devs;
	}();

	if (!devices || start >= all_devs.size() || devices > all_devs.size() - start)
	{
		return CELL_EINVAL;
	}

	std::copy_n(all_devs.begin() + start, devices, device_ids.get_ptr());

	return CELL_OK;
}

error_code sys_storage_configure_medium_event(u32 fd, u32 equeue_id, u32 c)
{
	sys_storage.notice("sys_storage_configure_medium_event(fd=0x%x, equeue_id=0x%x, c=0x%x)", fd, equeue_id, c);

	const auto handle = idm::get_unlocked<lv2_storage>(fd);

	if (!handle)
	{
		return CELL_ESRCH;
	}

	// c == 0 unregisters; anything else binds the queue to this handle.
	handle->medium_event_queue = c ? equeue_id : 0;

	// The image is mounted before the guest ever opens the drive, so by the
	// time anyone registers for medium events the disc is already "in". Report
	// it right away - waiting for an insertion that already happened would
	// leave the caller believing the drive is empty forever.
	if (handle->medium_event_queue && is_psx_disc_device(handle->device_id))
	{
		sys_storage_send_medium_insert_event();
	}

	return CELL_OK;
}

void sys_storage_send_medium_insert_event()
{
	// This used to send 4 then 8, which is Cobra's *eject* pair - medium removed
	// followed by drive not ready. A listener that gets told the disc left is
	// done: it will not probe the drive again, which is exactly the hang this
	// was meant to prevent. Insertion is 7 (medium detected) then 3 (medium
	// ready), and 3 carries the disc type in its parameter - announcing a disc
	// of type "none" is no more use than announcing an ejection.
	//
	// Both the constant and its placement are from webMAN-MOD's cobra sources
	// (include/mount/eject_insert.h, cobra/storage.h): the type is a 16-bit
	// DEVICE_TYPE_* value and it sits in the *high* word of the parameter,
	// `param = (u64)disctype << 32`. Putting it in the low word reads as type 0.
	static constexpr u64 device_type_psx_cd = 0xFF50;

	const std::pair<u64, u64> events[] = {{7, 0}, {3, device_type_psx_cd << 32}};

	idm::select<lv2_storage>([&](u32 /*id*/, lv2_storage& storage)
	{
		const u32 queue_id = storage.medium_event_queue;

		if (!queue_id || (storage.device_id & 0xFFFFF00FFFFFFFF) != BDVD_DRIVE)
		{
			return;
		}

		const auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(queue_id);

		if (!queue)
		{
			sys_storage.error("medium insert event: queue 0x%x is gone", queue_id);
			return;
		}

		for (const auto& [event, param] : events)
		{
			queue->send(0, event, param, storage.device_id);
		}

		sys_storage.success("Delivered medium insert events to queue 0x%x", queue_id);
	});
}

error_code sys_storage_set_medium_polling_interval()
{
	sys_storage.todo("sys_storage_set_medium_polling_interval()");

	return CELL_OK;
}

error_code sys_storage_create_region()
{
	sys_storage.todo("sys_storage_create_region()");

	return CELL_OK;
}

error_code sys_storage_delete_region()
{
	sys_storage.todo("sys_storage_delete_region()");

	return CELL_OK;
}

error_code sys_storage_execute_device_command(u32 fd, u64 cmd, vm::ptr<char> cmdbuf, u64 cmdbuf_size, vm::ptr<char> databuf, u64 databuf_size, vm::ptr<u32> driver_status)
{
	sys_storage.todo("sys_storage_execute_device_command(fd=0x%x, cmd=0x%llx, cmdbuf=*0x%x, cmdbuf_size=0x%llx, databuf=*0x%x, databuf_size=0x%llx, driver_status=*0x%x)", fd, cmd, cmdbuf, cmdbuf_size, databuf, databuf_size, driver_status);

	// cmd == 2 is get device info,
	// databuf, first byte 0 == status ok?
	// byte 1, if < 0 , not ata device

	// This used to answer READ TOC out of its own second implementation, which
	// ignored the MSF bit, the starting track and the allocation length, and
	// reported the lead-out (0xAA) as the last track. The command block is the
	// same one syscall 604 receives, so it goes through the same decoder.
	//
	// cmd == 1 is the SCSI selector here just as it is on 604: webMAN-MOD sends
	// START STOP UNIT through this syscall as
	// `system_call_7(616, dev_id, 1, cdb, 56, nullptr, 0, nullptr)`, which is
	// also where the 56-byte command buffer comes from.
	if (psx::disc* disc = psx_scsi_target(fd); disc && cmd == 1 && cmdbuf && cmdbuf_size >= 12)
	{
		const error_code result = exec_psx_scsi_command(disc, reinterpret_cast<const u8*>(cmdbuf.get_ptr()), cmdbuf_size,
			databuf ? reinterpret_cast<u8*>(databuf.get_ptr()) : nullptr, databuf_size);

		if (driver_status)
		{
			// Zero is "the drive reported no error"; anything else here is read
			// as a SCSI sense condition, not as the syscall's own return value.
			*driver_status = result == CELL_OK ? 0 : 1;
		}

		return result;
	}

	return CELL_OK;
}

error_code sys_storage_check_region_acl()
{
	sys_storage.todo("sys_storage_check_region_acl()");

	return CELL_OK;
}

error_code sys_storage_set_region_acl()
{
	sys_storage.todo("sys_storage_set_region_acl()");

	return CELL_OK;
}

error_code sys_storage_get_region_offset()
{
	sys_storage.todo("sys_storage_get_region_offset()");

	return CELL_OK;
}

error_code sys_storage_set_emulated_speed()
{
	sys_storage.todo("sys_storage_set_emulated_speed()");

	// todo: only debug kernel has this
	return CELL_ENOSYS;
}
