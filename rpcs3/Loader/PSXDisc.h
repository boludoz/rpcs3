#pragma once

#include "util/types.hpp"
#include "Utilities/File.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

// CD-ROM disc image backed by a CUE sheet (or a bare BIN/ISO), exposed as raw
// 2352-byte sectors plus a TOC.
//
// This models the disc the way a PS1 sees it, not the way a filesystem does:
// the PS1 firmware emulator drives an actual drive, so it asks for sectors and
// for the table of contents, and it needs the CD-DA tracks to be addressable
// or the games that stream their music off the disc (Wipeout, Tomb Raider,
// Tekken) come out silent.
namespace psx
{
	enum class track_type
	{
		mode1,   // 2048 bytes of user data per sector
		mode2,   // 2336 bytes of user data per sector (Form 1/2 decided per sector)
		audio,   // raw CD-DA
	};

	// Sectors are always handed out as full 2352-byte raw sectors. Images that
	// store cooked 2048-byte MODE1 sectors (a plain .iso) get a synthesized
	// sync pattern and header, since the emulator expects raw.
	inline constexpr u32 raw_sector_size = 2352;

	struct track
	{
		u32 number = 0;
		track_type type = track_type::mode1;

		// Bytes per sector as stored in the backing file.
		u32 sector_size = raw_sector_size;

		// Absolute start of the track on the disc, in sectors, counting the
		// 2-second (150 sector) lead-in the way MSF addressing does.
		u32 start_lba = 0;

		// Sector count, excluding any pregap that is not present in the file.
		u32 length = 0;

		// Pregap that exists only in the cue sheet (INDEX 00 missing from the
		// file). Reads that land here return silence/zeroes.
		u32 pregap = 0;

		// Index of the backing file in disc::m_files, and the byte offset of
		// this track inside it.
		usz file_index = 0;
		u64 file_offset = 0;

		bool is_audio() const { return type == track_type::audio; }
	};

	struct msf
	{
		u8 minute = 0;
		u8 second = 0;
		u8 frame = 0;
	};

	// 150 sectors of lead-in precede LBA 0 in MSF addressing.
	inline constexpr u32 lead_in_sectors = 150;

	msf lba_to_msf(u32 lba);
	u32 msf_to_lba(msf value);

	class disc
	{
	public:
		disc() = default;

		// Opens a .cue (parsed as a sheet) or a bare .bin/.img/.iso (treated as
		// a single data track). Returns false and leaves the object unusable if
		// the sheet is malformed or a referenced file is missing.
		bool open(const std::string& path);

		bool is_open() const { return !m_tracks.empty(); }

		const std::vector<track>& tracks() const { return m_tracks; }

		// Total sectors, i.e. the lead-out position.
		u32 total_sectors() const { return m_total_sectors; }

		// Reads one raw 2352-byte sector. Returns false past the lead-out or on
		// a read error; the buffer is zeroed either way, which is what a drive
		// reports for a gap.
		bool read_sector(u32 lba, void* out_2352);

		// Track containing `lba`, or nullptr past the lead-out.
		const track* track_at(u32 lba) const;

		// TOC entry as the drive reports it, matching Cobra's
		// ScsiTrackDescriptor layout so the emulator sees what it does on
		// hardware. adr_control is 0x14 for a data track and 0x10 for audio.
		struct toc_entry
		{
			u8 reserved = 0;
			u8 adr_control = 0;
			u8 track_number = 0;
			u8 reserved2 = 0;
			be_t<u32> track_start_addr{0};
		};

		std::vector<toc_entry> build_toc() const;

		// Bytes of formatted Q sub-channel data that follow a raw sector when
		// the READ CD command asks for it (CDB byte 10 == 0x02).
		static constexpr u32 q_subchannel_size = 16;

		// Synthesizes the Q sub-channel for `lba`. Real rips only carry this
		// separately (.sub files) and libcrypt-protected PAL games need the
		// genuine one, but a well-formed synthetic Q is what an unprotected
		// disc would report and is enough for everything else.
		void build_q_subchannel(u32 lba, void* out_16) const;

		// Where the "head" is, i.e. the last sector handed out. READ
		// SUB-CHANNEL reports the current position and there is no seek
		// command to track, so the last read is the only position there is.
		u32 last_lba() const { return m_last_lba; }

		const std::string& path() const { return m_path; }

		// Serial from the BOOT line of SYSTEM.CNF, e.g. "SCUS_949.00", or empty
		// if it cannot be found. Its third character is the region letter (U, E
		// or J), which is what the PS1 BIOS region patch has to be derived from
		// - patching a fixed value sets every disc to the same region.
		std::string boot_serial();

	private:
		bool parse_cue(const std::string& cue_path);
		bool open_single_file(const std::string& path);
		void finalize();

		// Loads a sibling .lsd or .sbi holding the genuine Q sub-channel of a
		// libcrypt-protected disc. Both store deliberately corrupted entries
		// for a handful of sectors; the game derives a key from them, so a
		// synthesized (correct) Q is exactly what makes those games fail.
		void load_subchannel_patches();

		std::string m_path;
		std::vector<fs::file> m_files;
		std::vector<track> m_tracks;
		u32 m_total_sectors = 0;

		// The emulator reads one sector at a time, so without read-ahead every
		// READ CD becomes its own file read - painful on Android storage.
		// webMAN's rawseciso keeps 48 sectors for the same reason.
		static constexpr u32 cache_sectors = 48;

		std::vector<u8> m_cache;
		usz m_cache_file = umax;
		u32 m_cache_first = 0;
		u32 m_cache_count = 0;
		u32 m_last_lba = 0;

		// LBA -> the 12 Q bytes that replace the synthesized ones.
		std::unordered_map<u32, std::array<u8, 12>> m_subq_patches;

		// A full .sub instead: 96 bytes of interleaved sub-channel per sector,
		// with Q at offset 12. Preferred over the patch formats when present -
		// it is the whole thing, not a diff, so nothing has to be inferred.
		fs::file m_subchannel_file;
	};

	// True for a .cue sheet, or for an image whose volume descriptor identifies
	// it as a PSX disc. Deliberately stricter than "the extension is .bin":
	// a PS3 .iso must not be captured by this.
	bool is_psx_image(const std::string& path);

	// Same test against an already-open image. Only the signature scan - a cue
	// sheet cannot be recognized this way, since it is text that names files
	// this overload has no way to reach.
	bool is_psx_image(const fs::file& file);

	// The mounted disc, i.e. the equivalent of Cobra's
	// sys_storage_ext_mount_psx_discfile. sys_storage serves the optical drive
	// out of this; nothing is mounted unless a PSX image was booted.
	bool mount(const std::string& path);
	void unmount();

	// Null when no disc is mounted. The returned pointer stays valid until
	// unmount(); callers must hold no reference across a boot.
	disc* mounted();
}
