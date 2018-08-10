/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * WiiWAD.cpp: Nintendo Wii WAD file reader.                               *
 *                                                                         *
 * Copyright (c) 2016-2018 by David Korth.                                 *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 ***************************************************************************/

#include "WiiWAD.hpp"
#include "librpbase/RomData_p.hpp"

#include "gcn_structs.h"
#include "wii_structs.h"
#include "wii_wad.h"

// librpbase
#include "librpbase/common.h"
#include "librpbase/byteswap.h"
#include "librpbase/TextFuncs.hpp"
#include "librpbase/SystemRegion.hpp"
#include "librpbase/file/IRpFile.hpp"

#include "libi18n/i18n.h"
using namespace LibRpBase;

// Decryption.
#include "librpbase/crypto/KeyManager.hpp"
#ifdef ENABLE_DECRYPTION
# include "librpbase/crypto/AesCipherFactory.hpp"
# include "librpbase/crypto/IAesCipher.hpp"
# include "librpbase/disc/CBCReader.hpp"
// Key verification.
# include "disc/WiiPartition.hpp"
#endif /* ENABLE_DECRYPTION */

// C includes. (C++ namespace)
#include <cassert>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>

// C++ includes.
#include <memory>
#include <sstream>
#include <string>
#include <vector>
using std::string;
using std::unique_ptr;
using std::vector;

namespace LibRomData {

class WiiWADPrivate : public RomDataPrivate
{
	public:
		WiiWADPrivate(WiiWAD *q, IRpFile *file);
		~WiiWADPrivate();

	private:
		typedef RomDataPrivate super;
		RP_DISABLE_COPY(WiiWADPrivate)

	public:
		// WAD structs.
		Wii_WAD_Header wadHeader;
		RVL_Ticket ticket;
		RVL_TMD_Header tmdHeader;
		Wii_Content_Bin_Header contentHeader;

		/**
		 * Round a value to the next highest multiple of 64.
		 * @param value Value.
		 * @return Next highest multiple of 64.
		 */
		template<typename T>
		static inline T toNext64(T val)
		{
			return (val + (T)63) & ~((T)63);
		}

#ifdef ENABLE_DECRYPTION
		// CBC reader for the main data area.
		CBCReader *cbcReader;
#endif /* ENABLE_DECRYPTION */
		// Key status.
		KeyManager::VerifyResult key_status;
};

/** WiiWADPrivate **/

WiiWADPrivate::WiiWADPrivate(WiiWAD *q, IRpFile *file)
	: super(q, file)
#ifdef ENABLE_DECRYPTION
	, cbcReader(nullptr)
#endif /* ENABLE_DECRYPTION */
	, key_status(KeyManager::VERIFY_UNKNOWN)
{
	// Clear the various structs.
	memset(&wadHeader, 0, sizeof(wadHeader));
	memset(&contentHeader, 0, sizeof(contentHeader));
}

WiiWADPrivate::~WiiWADPrivate()
{
#ifdef ENABLE_DECRYPTION
	delete cbcReader;
#endif /* ENABLE_DECRYPTION */
}

/** WiiWAD **/

/**
 * Read a Nintendo Wii WAD file.
 *
 * A WAD file must be opened by the caller. The file handle
 * will be dup()'d and must be kept open in order to load
 * data from the WAD file.
 *
 * To close the file, either delete this object or call close().
 *
 * NOTE: Check isValid() to determine if this is a valid ROM.
 *
 * @param file Open disc image.
 */
WiiWAD::WiiWAD(IRpFile *file)
	: super(new WiiWADPrivate(this, file))
{
	// This class handles application packages.
	RP_D(WiiWAD);
	d->className = "WiiWAD";
	d->fileType = FTYPE_APPLICATION_PACKAGE;

	if (!d->file) {
		// Could not dup() the file handle.
		return;
	}

	// Read the WAD header.
	d->file->rewind();
	size_t size = d->file->read(&d->wadHeader, sizeof(d->wadHeader));
	if (size != sizeof(d->wadHeader)) {
		delete d->file;
		d->file = nullptr;
		return;
	}

	// Check if this WAD file is supported.
	DetectInfo info;
	info.header.addr = 0;
	info.header.size = sizeof(d->wadHeader);
	info.header.pData = reinterpret_cast<const uint8_t*>(&d->wadHeader);
	info.ext = nullptr;	// Not needed for WiiWAD.
	info.szFile = d->file->size();
	d->isValid = (isRomSupported_static(&info) >= 0);
	if (!d->isValid) {
		delete d->file;
		d->file = nullptr;
		return;
	}

	// Read the ticket and TMD.
	// TODO: Verify ticket/TMD sizes.
	unsigned int addr = WiiWADPrivate::toNext64(be32_to_cpu(d->wadHeader.header_size)) +
			    WiiWADPrivate::toNext64(be32_to_cpu(d->wadHeader.cert_chain_size));
	size = d->file->seekAndRead(addr, &d->ticket, sizeof(d->ticket));
	if (size != sizeof(d->ticket)) {
		// Seek and/or read error.
		d->isValid = false;
		delete d->file;
		d->file = nullptr;
		return;
	}
	addr += WiiWADPrivate::toNext64(be32_to_cpu(d->wadHeader.ticket_size));
	size = d->file->seekAndRead(addr, &d->tmdHeader, sizeof(d->tmdHeader));
	if (size != sizeof(d->tmdHeader)) {
		// Seek and/or read error.
		d->isValid = false;
		delete d->file;
		d->file = nullptr;
		return;
	}

#ifdef ENABLE_DECRYPTION
	// Initialize the CBC reader for the main data area.
	// TODO: Determine key index and debug vs. retail by reading the TMD.
	// TODO: WiiVerifyKeys class.
	KeyManager *const keyManager = KeyManager::instance();
	assert(keyManager != nullptr);

	// Key verification data.
	// TODO: Move out of WiiPartition and into WiiVerifyKeys?
	const uint8_t *const verifyData = WiiPartition::encryptionVerifyData_static(WiiPartition::Key_Rvl_Common);
	assert(verifyData != nullptr);

	// Get and verify the key.
	KeyManager::KeyData_t keyData;
	d->key_status = keyManager->getAndVerify("rvl-common", &keyData, verifyData, 16);
	if (d->key_status == KeyManager::VERIFY_OK) {
		// Create a cipher to decrypt the title key.
		IAesCipher *cipher = AesCipherFactory::create();

		// Initialize parameters for title key decryption.
		// TODO: Error checking.
		// Parameters:
		// - Chaining mode: CBC
		// - IV: Title ID (little-endian)
		cipher->setChainingMode(IAesCipher::CM_CBC);
		cipher->setKey(keyData.key, keyData.length);
		// Title key IV: High 8 bytes are the title ID (in big-endian), low 8 bytes are 0.
		uint8_t iv[16];
		memcpy(iv, &d->ticket.title_id.id, sizeof(d->ticket.title_id.id));
		memset(&iv[8], 0, 8);
		cipher->setIV(iv, sizeof(iv));
		
		// Decrypt the title key.
		uint8_t title_key[16];
		memcpy(title_key, d->ticket.enc_title_key, sizeof(d->ticket.enc_title_key));
		cipher->decrypt(title_key, sizeof(title_key));

		// Data area IV:
		// - First two bytes are the big-endian content index.
		// - Remaining bytes are zero.
		// - TODO: Read the TMD content table. For now, assuming index 0.
		memset(iv, 0, sizeof(iv));

		// Create a CBC reader to decrypt the data section.
		addr += WiiWADPrivate::toNext64(be32_to_cpu(d->wadHeader.tmd_size));
		d->cbcReader = new CBCReader(d->file, addr, be32_to_cpu(d->wadHeader.data_size), title_key, iv);

		// TODO: Verify some known data?
	}
#else /* !ENABLE_DECRYPTION */
	// Cannot decrypt anything...
	d->key_status = KeyManager::VERIFY_NO_SUPPORT;
#endif /* ENABLE_DECRYPTION */
}

/** ROM detection functions. **/

/**
 * Is a ROM image supported by this class?
 * @param info DetectInfo containing ROM detection information.
 * @return Class-specific system ID (>= 0) if supported; -1 if not.
 */
int WiiWAD::isRomSupported_static(const DetectInfo *info)
{
	assert(info != nullptr);
	assert(info->header.pData != nullptr);
	assert(info->header.addr == 0);
	if (!info || !info->header.pData ||
	    info->header.addr != 0 ||
	    info->header.size < sizeof(Wii_WAD_Header))
	{
		// Either no detection information was specified,
		// or the header is too small.
		return -1;
	}

	// Check for the correct header fields.
	const Wii_WAD_Header *wadHeader = reinterpret_cast<const Wii_WAD_Header*>(info->header.pData);
	if (wadHeader->header_size != cpu_to_be32(sizeof(*wadHeader))) {
		// WAD header size is incorrect.
		return -1;
	}

	// Check WAD type.
	if (wadHeader->type != cpu_to_be32(WII_WAD_TYPE_Is) &&
	    wadHeader->type != cpu_to_be32(WII_WAD_TYPE_ib) &&
	    wadHeader->type != cpu_to_be32(WII_WAD_TYPE_Bk))
	{
		// WAD type is incorrect.
		return -1;
	}

	// Verify the ticket size.
	// TODO: Also the TMD size.
	if (be32_to_cpu(wadHeader->ticket_size) < sizeof(RVL_Ticket)) {
		// Ticket is too small.
		return -1;
	}
	
	// Check the file size to ensure we have at least the IMET section.
	unsigned int expected_size = WiiWADPrivate::toNext64(be32_to_cpu(wadHeader->header_size)) +
				     WiiWADPrivate::toNext64(be32_to_cpu(wadHeader->cert_chain_size)) +
				     WiiWADPrivate::toNext64(be32_to_cpu(wadHeader->ticket_size)) +
				     WiiWADPrivate::toNext64(be32_to_cpu(wadHeader->tmd_size)) +
				     sizeof(Wii_Content_Bin_Header);
	if (expected_size > info->szFile) {
		// File is too small.
		return -1;
	}

	// This appears to be a Wii WAD file.
	return 0;
}

/**
 * Is a ROM image supported by this object?
 * @param info DetectInfo containing ROM detection information.
 * @return Class-specific system ID (>= 0) if supported; -1 if not.
 */
int WiiWAD::isRomSupported(const DetectInfo *info) const
{
	return isRomSupported_static(info);
}

/**
 * Get the name of the system the loaded ROM is designed for.
 * @param type System name type. (See the SystemName enum.)
 * @return System name, or nullptr if type is invalid.
 */
const char *WiiWAD::systemName(unsigned int type) const
{
	RP_D(const WiiWAD);
	if (!d->isValid || !isSystemNameTypeValid(type))
		return nullptr;

	// Wii has the same name worldwide, so we can
	// ignore the region selection.
	static_assert(SYSNAME_TYPE_MASK == 3,
		"WiiWAD::systemName() array index optimization needs to be updated.");

	static const char *const sysNames[4] = {
		"Nintendo Wii", "Wii", "Wii", nullptr
	};

	return sysNames[type & SYSNAME_TYPE_MASK];
}

/**
 * Get a list of all supported file extensions.
 * This is to be used for file type registration;
 * subclasses don't explicitly check the extension.
 *
 * NOTE: The extensions do not include the leading dot,
 * e.g. "bin" instead of ".bin".
 *
 * NOTE 2: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *WiiWAD::supportedFileExtensions_static(void)
{
	static const char *const exts[] = { ".wad" };
	return exts;
}

/**
 * Get a list of all supported file extensions.
 * This is to be used for file type registration;
 * subclasses don't explicitly check the extension.
 *
 * NOTE: The extensions do not include the leading dot,
 * e.g. "bin" instead of ".bin".
 *
 * NOTE 2: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *WiiWAD::supportedFileExtensions(void) const
{
	return supportedFileExtensions_static();
}

/**
 * Load field data.
 * Called by RomData::fields() if the field data hasn't been loaded yet.
 * @return Number of fields read on success; negative POSIX error code on error.
 */
int WiiWAD::loadFieldData(void)
{
	RP_D(WiiWAD);
	if (d->fields->isDataLoaded()) {
		// Field data *has* been loaded...
		return 0;
	} else if (!d->file || !d->file->isOpen()) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// Unknown file type.
		return -EIO;
	}

	// WAD headers are read in the constructor.
	const RVL_TMD_Header *const tmdHeader = &d->tmdHeader;
	d->fields->reserve(3);	// Maximum of 3 fields.

	if (d->key_status != KeyManager::VERIFY_OK) {
		// Unable to get the decryption key.
		const char *err = KeyManager::verifyResultToString(d->key_status);
		if (!err) {
			err = C_("WiiWAD", "Unknown error. (THIS IS A BUG!)");
		}
		d->fields->addField_string(C_("WiiWAD", "Warning"),
			err, RomFields::STRF_WARNING);
		return (int)d->fields->count();
	}

	// Title ID.
	// TODO: Make sure the ticket title ID matches the TMD title ID.
	d->fields->addField_string(C_("WiiWAD", "Title ID"),
		rp_sprintf("%08X-%08X", be32_to_cpu(tmdHeader->title_id.hi), be32_to_cpu(tmdHeader->title_id.lo)));

	// Game ID.
	// NOTE: Only displayed if TID lo is all alphanumeric characters.
	// TODO: Only for certain TID hi?
	if (isalnum(tmdHeader->title_id.u8[4]) &&
	    isalnum(tmdHeader->title_id.u8[5]) &&
	    isalnum(tmdHeader->title_id.u8[6]) &&
	    isalnum(tmdHeader->title_id.u8[7]))
	{
		// Print the game ID.
		// TODO: Is the publisher code available anywhere?
		d->fields->addField_string(C_("WiiWAD", "Game ID"),
			rp_sprintf("%.4s", reinterpret_cast<const char*>(&tmdHeader->title_id.u8[4])));
	}

	// Required IOS version.
	const uint32_t ios_lo = be32_to_cpu(tmdHeader->sys_version.lo);
	if (tmdHeader->sys_version.hi == cpu_to_be32(0x00000001) &&
	    ios_lo > 2 && ios_lo < 0x300)
	{
		// Standard IOS slot.
		d->fields->addField_string(C_("WiiWAD", "IOS Version"),
			rp_sprintf("IOS%u", ios_lo));
	} else {
		// Non-standard IOS slot.
		// Print the full title ID.
		d->fields->addField_string(C_("WiiWAD", "IOS Version"),
			rp_sprintf("%08X-%08X", be32_to_cpu(tmdHeader->sys_version.hi), be32_to_cpu(tmdHeader->sys_version.lo)));
	}
	
	// TODO: Decrypt content.bin to get the actual data.

	// Finished reading the field data.
	return (int)d->fields->count();
}

}
