/*
 * Copyright © 2026 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terascale_device_name.h"

#include "util/macros.h"

#include <stddef.h>
#include <stdint.h>

/* The R8xx and R9xx chip names are from AMD's device ID headers:
 * https://github.com/GPUOpen-Archive/CodeXL/tree/8806bd32c5970d5f7240d97786a86d2849346938/CodeXL/Components/ShaderAnalyzer/AMDTBackEnd/Include/Common/asic_reg
 * See `evergreen_id.h`, `sumo_id.h`, `northernisland_id.h`, `tn_id.h`
 * (copyright (c) 2016 Advanced Micro Devices, Inc., licensed under the terms of the MIT license).
 *
 * In this file, the chip names were cleaned up for consistency. The GL suffix is set based on
 * whether the chip actually ended up primarily being used in FirePro graphics cards rather than
 * Radeon, and suffixes like XT, PRO and LE are placed first because they are most descriptive of
 * the performance of each chip within a single family. Slash-separated subtier suffixes are listed
 * in descending order of performance like in most cases in AMD's device ID headers.
 *
 * It's not always possible to determine the product name for a given device exactly, however. In
 * many cases, the same chip was installed in graphics cards with different product names, or the
 * same product name is used with multiple chips, including from different families and generations.
 *
 * Therefore, when the exact product name is known for the device or the subsystem, it's used to
 * reduce user's confusion. The product names are taken primarily from official sources, such as AMD
 * Windows drivers - not from community databases (like pci.ids), unless the correctness of the
 * names from them can be verified easily. For the R5 graphics cards, the broader "Series" naming
 * may be used if that's how they're named in the official sources, since they're differentiated
 * from the rest of Evergreen and Northern Islands anyway.
 *
 * However, when it's not trivial to obtain the exact product name, the name of an existing product
 * based on the given chip (or a series, if many product names are commonly used for the same chip)
 * that's expected to be more representative of its performance is chosen.
 *
 * In the Windows drivers, it's common that broad names like "HD xx00 Series" are used. Generally,
 * in this file, more precise names are specified instead where available, chosen according to the
 * following recommendations:
 * - The earliest products using the chip are primarily selected as the generic name, being most
 *   representative of the chip generation. For Evergreen chips, the HD 5xxx name is preferred, and
 *   for Northern Islands, the HD 6xxx name is. HD 7xxx/8xxx can easily be confused with GCN chips.
 *   The exception is when the chip was primarily used in more recent graphics cards - while Turks
 *   XT/PRO are usual Northern Islands HD 6xxx chips, Turks XT2/PRO2 were released specifically as
 *   HD 7xxx.
 * - In the chosen product line, the more common product name for the chip is used. Sometimes the
 *   selection here is ambiguous as one chip may be frequently used in multiple products. This may
 *   also occur between multiple product lines. In this case, multiple names are specified,
 *   separated with a slash, in ascending order (because the earlier product lines are preferred,
 *   and for consistency with the Windows driver naming). For cross-generation ("Mobility Radeon HD
 *   5xxx" and "Radeon HD 6xxxM") mobile chips, the naming should be "Mobility Radeon HD 5xxx/6xxxM"
 *   (note that if a Mobility HD 5xxx chip is used in both mobile and desktop products, which is
 *   often the case, for simplicity, just using the word "Mobility" for the whole list and
 *   specifying the 5xxx numbers for both mobile and desktop graphics cards without differentiating
 *   between the two, as the same numbers are frequently used for both).
 * - Higher- and middle-tier chip families are not rebranded as often as lower-tier and mobile ones.
 *   For them, often XT refers to HD xx70, and PRO is HD xx50, so such names are used if they meet
 *   the other criteria. However, for mobile graphics cards, the third digit may vary greatly within
 *   a single chip, while the second digit may reflect the performance subtier in the chip family.
 *   In this case, "HD xx00 Series" naming may be used - however, it's preferable, though not
 *   necessary, that different chips still use different product names. For example, Whistler XT may
 *   be named "HD 6700M Series", while Whistler PRO/LP would be "HD 6600M/6700M Series".
 *
 * The lists of existing product names (though missing a few as of July 2026) can be found at:
 * https://www.amd.com/en/support/download/drivers.html#search-browse-drivers
 *
 * The primary sources of the product names are:
 * - Windows driver 15.301.1901.0000 (C0318062) from 2016.
 * - Windows driver 15.201.2401.0000 (C0296716, C7296716, CU296716) from 2015 primarily for FirePro.
 * - Windows driver 13.150.0.0000 (CU158142) from 2013 - contains APU chip names, but no subsystems.
 *
 * Additional references can be found using the Internet Archive Wayback Machine under:
 * http://developer.amd.com/gpu_assets/
 * http://ati.amd.com/developer/
 * in the "ATI_Device_ID*.txt" and "ATI_Vendor_ID*.txt" files, and also in an archived copy of:
 * http://ati.amd.com/developer/vendorid.html
 * These files, however, do not include subsystem IDs.
 *
 * Only AMD product name formats are used in this file for consistency and ease of communication
 * between users and driver and application developers via issue and benchmarking reports. If the
 * manufacturer's branding includes the AMD product number (such as the ASUS EAH series), it's
 * canonicalized to the AMD format, otherwise (like for the Barco MXRT series) the manufacturer's
 * product name is omitted. The manufacturer's product name and/or additional suffixes, however, may
 * be included alongside the AMD name when it may be desirable to reflect the distinct qualities of
 * the specific subsystem, such as for dual-chip graphics cards like the ASUS ARES.
 *
 * Currently this list includes only R8xx and R9xx devices, because it was initially created for the
 * Vulkan implementation.
 */

struct terascale_device_name_subsystem {
   /* Subsystem vendor ID in the upper 16 bits, subsystem device ID in the lower 16 bits. */
   uint32_t id;

   /* NULL if this is an array terminator. */
   const char *product_name;
};

struct terascale_device_name_device {
   struct terascale_device_name generic_name;

   /* NULL if there are no subsystem-specific names. */
   const struct terascale_device_name_subsystem *subsystems;
};

/* Northern Islands. */

/* clang-format off */

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6738[] = {
   {0x17AF201A, "Radeon HD 6870 X2"},
   {0x17AF201B, "Radeon HD 6870 X2"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6739[] = {
   {0x17873000, "Radeon HD 6850 X2"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x673E[] = {
   {0x148C7720, "Radeon HD 7720"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6742[] = {
   /* The devices with the subsystem device ID 0x6570 are named "Radeon HD 6500 Series" in the
    * Windows driver 15.301.1901.0000.
    */
   {0x10436570, "Radeon HD 6570"},
   {0x14585570, "Radeon HD 5570"},
   {0x14586570, "Radeon HD 6570"},
   /* 1462:250B is "Radeon HD 6500 Series" in the Windows driver 15.301.1901.0000. */
   {0x14626570, "Radeon HD 6570"},
   {0x148C5570, "Radeon HD 5570"},
   {0x148C6570, "Radeon HD 6570"},
   {0x16825230, "Radeon R5 230"},
   {0x16826450, "Radeon HD 6450"},
   {0x16826570, "Radeon HD 6570"},
   {0x174B5570, "Radeon HD 5570"},
   {0x174B6570, "Radeon HD 6570"},
   {0x174B7570, "Radeon HD 7570"},
   {0x174B8510, "Radeon HD 8510"},
   {0x174B8570, "Radeon HD 8570"},
   {0x174BE181, "Radeon HD 6570"},
   {0x17875570, "Radeon HD 5570"},
   {0x17876570, "Radeon HD 6570"},
   {0x1787A230, "Radeon R5 230"},
   {0x1787A450, "Radeon HD 6450"},
   {0x17AF6570, "Radeon HD 6570"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6758[] = {
   {0x1462250A, "Radeon HD 7670"},
   {0x148C7670, "Radeon HD 7670"},
   {0x15457670, "Radeon HD 7670"},
   {0x16823300, "Radeon HD 7670"},
   {0x174B7670, "Radeon HD 7670"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6759[] = {
   {0x14622509, "Radeon HD 7570"},
   {0x148C7570, "Radeon HD 7570"},
   {0x16423A67, "Radeon HD 6570"},
   {0x16823280, "Radeon HD 7570"},
   {0x16823530, "Radeon HD 8550"},
   {0x16825230, "Radeon R5 230"},
   {0x16826450, "Radeon HD 6450"},
   {0x174B7570, "Radeon HD 7570"},
   {0x174B8550, "Radeon HD 8550"},
   {0x174B8570, "Radeon HD 8550"},
   {0x1787A230, "Radeon R5 230"},
   {0x1787A450, "Radeon HD 6450"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x675D[] = {
   {0x16823530, "Radeon HD 8550"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x675F[] = {
   {0x148C6510, "Radeon HD 6510"},
   {0x148C6530, "Radeon HD 6530"},
   {0x148C7510, "Radeon HD 7510"},
   {0x148C8510, "Radeon HD 8510"},
   {0x15457570, "Radeon HD 7570"},
   {0x16823520, "Radeon HD 8510"},
   {0x16825230, "Radeon R5 230"},
   {0x16826450, "Radeon HD 6450"},
   {0x174B6510, "Radeon HD 6510"},
   {0x174B7510, "Radeon HD 7510"},
   {0x174B8510, "Radeon HD 8510"},
   {0x1787A230, "Radeon R5 230"},
   {0x1787A450, "Radeon HD 6450"},
   {0x17AF8510, "Radeon HD 8510"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6761[] = {
   {0x17873000, "Radeon HD 6450"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6778[] = {
   {0x1462B491, "Radeon HD 8470"},
   {0x1462B492, "Radeon HD 8470"},
   {0x1462B493, "Radeon HD 8470"},
   {0x1462B499, "Radeon R5 235"},
   {0x16423C65, "Radeon HD 8470"},
   {0x16423C75, "Radeon HD 8470"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6779[] = {
   {0x10433013, "Radeon R5 230 Series"},
   {0x10433015, "Radeon R5 230 Series"},
   {0x10433017, "Radeon R5 230 Series"},
   {0x10433019, "Radeon HD 5450"},
   {0x1043301B, "Radeon HD 5450"},
   {0x1043301D, "Radeon HD 5450"},
   {0x1043301F, "Radeon HD 5450"},
   {0x145822A2, "Radeon R5 200 Series"},
   {0x145822A3, "Radeon R5 200 Series"},
   {0x1462212C, "Radeon R5 200 Series"},
   {0x14622346, "Radeon HD 7450"},
   {0x14622347, "Radeon R5 200 Series"},
   {0x14628A9D, "Radeon R5 200 Series"},
   {0x148C5230, "Radeon R5 200 Series"},
   {0x148C7450, "Radeon HD 7450"},
   {0x148C8450, "Radeon HD 8450"},
   {0x16823200, "Radeon HD 7450"},
   {0x16825230, "Radeon R5 200 Series"},
   {0x174B7450, "Radeon HD 7450"},
   {0x174B8450, "Radeon HD 8450"},
   {0x174BA004, "Radeon R5 200 Series"},
   {0x174BA006, "Radeon HD 7470"},
   {0x174BA007, "Radeon HD 5450"},
   {0x174BE164, "Radeon HD 6450"},
   {0x17872300, "Radeon R5 230"},
   {0x17873000, "Radeon R5 200 Series"},
   {0x17873008, "Radeon HD 5450"},
   {0x17875230, "Radeon R5 200 Series"},
   {0x17AF8450, "Radeon HD 8450"},
   {0x1B0A90BB, "Radeon HD 7450A"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x677B[] = {
   /* "Radeon(TM) HD 6400 Series" from the Windows driver 15.301.1901.0000 changed to "Radeon HD
    * 6450" because it appears to be the only graphics card in the HD 64xx series, and HD xx50
    * usually corresponds to PRO (this is PRO2).
    */
   {0x14622129, "Radeon HD 7450"},
   {0x1462212E, "Radeon R5 200 Series"},
   {0x1462212F, "Radeon HD 6450"},
   {0x14628099, "Radeon R5 230"},
   {0x1462809A, "Radeon HD 6450"},
   {0x174B3000, "Radeon HD 7450"},
   {0x17AF7450, "Radeon HD 7450"},
   {},
};

/* clang-format on */

static const struct terascale_device_name_device terascale_device_name_devices_0x6700[] = {
   /* Cayman: 0x6700... */

   [0x00] = {{"Cayman XT GL Cliffhanger"}},
   [0x01] = {{"Cayman XT GL Moonracer"}},
   [0x02] = {{"Cayman XT GL Spellbreaker"}},
   [0x03] = {{"Cayman XT GL"}},
   [0x04] = {{"Cayman PRO GL Starsaber", "FirePro V7900"}},
   [0x05] = {{"Cayman PRO GL"}},
   [0x06] = {{"Cayman GL Doubleheader"}},
   [0x07] = {{"Cayman LE GL Jetstorm", "FirePro V5900"}},
   [0x08] = {{"Cayman GL"}},
   [0x09] = {{"Cayman GL"}},
   [0x18] = {{"Cayman XT", "Radeon HD 6970"}},
   [0x19] = {{"Cayman PRO", "Radeon HD 6950"}},
   [0x1C] = {{"Antilles PRO"}},
   [0x1D] = {{"Antilles XT", "Radeon HD 6990"}},
   [0x1F] = {{"Cayman CE", "Radeon HD 6930"}},

   /* Barts: 0x6720... */

   [0x20] = {{"Blackcomb XT/PRO", "Radeon HD 6970M/6990M"}},
   [0x21] = {{"Blackcomb LP"}},
   [0x22] = {{"Barts GL2"}},
   [0x23] = {{"Barts GL3"}},
   [0x24] = {{"Blackcomb XT/PRO Gemini"}},
   [0x25] = {{"Blackcomb LP Gemini"}},
   [0x26] = {{"Barts GL6"}},
   [0x27] = {{"Barts GL7"}},
   [0x28] = {{"Barts XT GL"}},
   [0x29] = {{"Barts PRO GL"}},
   [0x30] = {{"Blackcomb AIO"}},
   [0x38] = {{"Barts XT", "Radeon HD 6870"}, terascale_device_name_subsystems_0x6738},
   [0x39] = {{"Barts PRO", "Radeon HD 6850"}, terascale_device_name_subsystems_0x6739},
   [0x3E] = {{"Barts LE", "Radeon HD 6790"}, terascale_device_name_subsystems_0x673E},

   /* Turks: 0x6740... */

   [0x40] = {{"Whistler XT", "Radeon HD 6700M Series"}},
   [0x41] = {{"Whistler PRO/LP", "Radeon HD 6600M/6700M Series"}},
   /* The generic name is "Radeon HD 7500/7600 Series" in the Windows driver 15.301.1901.0000, but
    * many subsystems (primarily HD 6570) are named "Radeon HD 6500 Series", and this is more
    * consistent with other Whistler chips.
    */
   [0x42] = {{"Whistler LE", "Radeon HD 6500 Series"}, terascale_device_name_subsystems_0x6742},
   [0x43] = {{"Whistler CSP PRO", "Radeon E6760"}},
   [0x44] = {{"Whistler XT/PRO Gemini"}},
   [0x45] = {{"Whistler LP 256 G5"}},
   [0x46] = {{"Turks GL6"}},
   [0x47] = {{"Turks GL7"}},
   [0x48] = {{"Turks GL8"}},
   [0x49] = {{"Turks GL9", "FirePro V4900"}},
   [0x4A] = {{"Turks GL", "FirePro V3900"}},
   [0x50] = {{"Onega", "Radeon HD 6600A Series"}},
   [0x51] = {{"Onega2", "Radeon HD 7600A Series"}},
   [0x58] = {{"Turks XT", "Radeon HD 6670"}, terascale_device_name_subsystems_0x6758},
   [0x59] = {{"Turks PRO", "Radeon HD 6570"}, terascale_device_name_subsystems_0x6759},
   [0x5B] = {{"Turks XT2", "Radeon HD 7670"}},
   [0x5D] = {{"Turks PRO2", "Radeon HD 7570"}, terascale_device_name_subsystems_0x675D},
   /* "Radeon HD 5500 Series" in the Windows driver 15.301.1901.0000, but HD 5570 appears to be the
    * only Turks graphics card in the HD 55xx series.
    */
   [0x5F] = {{"Turks LE", "Radeon HD 5570"}, terascale_device_name_subsystems_0x675F},

   /* Caicos: 0x6760... */

   [0x60] = {{"Seymour XT/PRO", "Radeon HD 6400M/7400M Series"}},
   [0x61] = {{"Seymour LP", "Radeon HD 6430M"}, terascale_device_name_subsystems_0x6761},
   [0x62] = {{"Caicos GL2"}},
   [0x63] = {{"Caicos CSP512 G5", "Radeon E6460"}},
   [0x64] = {{"Seymour XT/PRO Gemini"}},
   [0x65] = {{"Seymour LP Gemini"}},
   [0x66] = {{"Caicos GL6"}},
   [0x67] = {{"Caicos GL7"}},
   [0x68] = {{"Caicos PRO GL"}},
   [0x70] = {{"Caspian PRO", "Radeon HD 6400A/7400A Series"}},
   [0x71] = {{"Caicos XTX", "Radeon HD 8490"}},
   [0x72] = {{"Caspian PRO2", "Radeon HD 7400A Series"}},
   /* "Radeon HD 7000 series" in the Windows driver 15.301.1901.0000, but HD 7450 is Caicos PRO
    * (0x6779) or Caicos PRO2 (0x677B), and HD 6xxx is usually rebranded as the corresponding HD
    * 7xxx and HD 8xxx.
    */
   [0x78] = {{"Caicos XT", "Radeon HD 7470/8470"}, terascale_device_name_subsystems_0x6778},
   [0x79] = {{"Caicos PRO", "Radeon HD 6450"}, terascale_device_name_subsystems_0x6779},
   /* "Radeon HD 7400 Series" in the Windows driver 15.301.1901.0000, but HD 7450 and HD 7470 appear
    * to be the only graphics cards in the HD 74xx series, and HD 7470 is Caicos XT.
    */
   [0x7B] = {{"Caicos PRO2", "Radeon HD 7450"}, terascale_device_name_subsystems_0x677B},
};

/* Northern Islands Lombok / Thames (mobile Turks). */

/* clang-format off */

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6843[] = {
   {0x148C6570, "Radeon HD 6570"},
   {0x17876570, "Radeon HD 6570"},
   {},
};

/* clang-format on */

static const struct terascale_device_name_device terascale_device_name_devices_0x6840[] = {
   [0x00] = {{"Thames XT", "Radeon HD 7600M Series"}},
   [0x01] = {{"Thames PRO", "Radeon HD 7500M/7600M Series"}},
   [0x02] = {{"Thames LE", "Radeon HD 7000M Series"}},
   [0x03] = {{"Whistler XTX", "Radeon HD 7670M"}, terascale_device_name_subsystems_0x6843},
   [0x09] = {{"Lombok XT GL"}},
   [0x10] = {{"Lombok AIO"}},
   [0x18] = {{"Lombok XT", "Radeon HD 7400 Series"}},
   [0x19] = {{"Lombok PRO", "Radeon HD 7400 Series"}},
};

/* Evergreen. */

/* clang-format off */

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6889[] = {
   {0x10020301, "FirePro V7800P"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x688A[] = {
   {0x1002030C, "FirePro V9800P"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6898[] = {
   /* 1002:00D0 is "Radeon HD 6800 Series" in the Windows driver 15.301.1901.0000. */
   {0x100200D0, "Radeon HD 6870"},
   /* 174B:6870 is "Radeon HD 6870" in the Windows driver 15.301.1901.0000, but more clearly
    * differentiate from Barts XT.
    */
   {0x174B6870, "Radeon HD 6870 1600SP Edition"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x6899[] = {
   {0x174B237B, "Radeon HD 5850 X2"},
   /* 174B:6850 is "Radeon HD 6850" in the Windows driver 15.301.1901.0000, but more clearly
    * differentiate from Barts PRO.
    */
   {0x174B6850, "Radeon HD 6850 1440SP Edition"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x689C[] = {
   {0x10430352, "Radeon HD 5870 X2 (ASUS ARES)"},
   /* 1043:039E is ASUS EAH5870. */
   {0x1043039E, "Radeon HD 5870"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68B8[] = {
   /* 1002:3000 is "Radeon HD 6700 Series" in the Windows driver 15.301.1901.0000. */
   {0x10023000, "Radeon HD 6770"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68BE[] = {
   {0x148C3000, "Radeon HD 6750"},
   {0x16423982, "Radeon HD 6750"},
   {0x174B3000, "Radeon HD 6750"},
   {0x17AF3000, "Radeon HD 6750"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68BF[] = {
   {0x148C3000, "Radeon HD 6750"},
   {0x174B6750, "Radeon HD 6750"},
   /* 1787:3000 is "Radeon HD 6700 Green Edition" in the Windows driver 15.301.1901.0000, but HD
    * 6770 Green Edition is the exact name of the graphics card.
    */
   {0x17873000, "Radeon HD 6770 Green Edition"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68C7[] = {
   {0x174B5570, "Radeon HD 5570"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68D8[] = {
   {0x174B5690, "Radeon HD 5690"},
   {0x174B5730, "Radeon HD 5730"},
   {0x17873000, "Radeon HD 5730"},
   {0x17AF3010, "Radeon HD 5730"},
   {0x17AF3011, "Radeon HD 5690"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68D9[] = {
   {0x148C3000, "Radeon HD 6510"},
   {0x148C3001, "Radeon HD 6610"},
   {0x15457570, "Radeon HD 7570"},
   {0x174B3000, "Radeon HD 6510"},
   {0x174B6510, "Radeon HD 6510"},
   {0x174B6610, "Radeon HD 6610"},
   {0x17873000, "Radeon HD 6510"},
   {0x17AF3000, "Radeon HD 6510"},
   {0x17AF3010, "Radeon HD 5630"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68DA[] = {
   {0x148C3000, "Radeon HD 6390"},
   {0x148C3001, "Radeon HD 6490"},
   {0x15457570, "Radeon HD 7570"},
   {0x174B3000, "Radeon HD 6390"},
   {0x174B5570, "Radeon HD 5570"},
   {0x174B5630, "Radeon HD 5630"},
   {0x174B6490, "Radeon HD 6490"},
   {0x17873000, "Radeon HD 5630"},
   {0x17AF3000, "Radeon HD 6390"},
   {0x17AF3010, "Radeon HD 5630"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68E0[] = {
   {0x1682304E, "Radeon HD 5450"},
   {0x16825220, "Radeon R5 220"},
   /* 174B:E204 is "Radeon HD 5400 Series" in the Windows driver 15.301.1901.0000, but it's not
    * clear which graphics card it exactly is, so don't use a name that represents its performance
    * less precisely.
    */
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68E1[] = {
   /* 1092:6000, 1458:21D8, 1458:21E2, 1458:21F1, 1458:2202, 1458:2548, 1462:2342, 1462:2343,
    * 148C:3000, 1682:3000, 1682:6000, 174B:3000, 174B:6000, 1787:3000, 17AF:3000 are named "Radeon
    * HD 5400 Series" in the Windows driver 15.301.1901.0000, but it's not clear which graphics
    * cards they exactly are, so don't use a name that represents their performance less precisely.
    * The primary difference likely is that those graphics cards are desktop variants of Mobility
    * Radeon HD 5430, and therefore shouldn't have the Mobility name.
    * 1458:254A is also specifically named "Mobility Radeon HD 5000 Series", but this name matches
    * the generic name.
    */
   {0x10433000, "Radeon HD 5450"},
   {0x148C3001, "Radeon HD 6230"},
   {0x148C3002, "Radeon HD 6250"},
   {0x148C3003, "Radeon HD 6350"},
   {0x148C7350, "Radeon HD 7350"},
   {0x148C8350, "Radeon HD 8350"},
   {0x15455450, "Radeon HD 5450"},
   {0x15457350, "Radeon HD 7350"},
   {0x16827350, "Radeon HD 7350"},
   {0x174B5470, "Radeon HD 5470"},
   {0x174B6230, "Radeon HD 6230"},
   {0x174B6350, "Radeon HD 6350"},
   {0x174B7350, "Radeon HD 7350"},
   {0x17AF3001, "Radeon HD 6230"},
   {0x17AF3014, "Radeon HD 6350"},
   {0x17AF3015, "Radeon HD 7350"},
   {0x17AF8350, "Radeon HD 8350"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68E4[] = {
   {0x15457350, "Radeon HD 7350"},
   {0x174B5450, "Radeon HD 5450"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68E5[] = {
   {0x148C5450, "Radeon HD 5450"},
   {0x148C6350, "Radeon HD 6350"},
   {0x148C7350, "Radeon HD 7350"},
   {0x148C8350, "Radeon HD 8350"},
   {0x15457350, "Radeon HD 7350"},
   {0x174BE164, "Radeon HD 5450"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68F9[] = {
   {0x103C2126, "Radeon HD 6350"},
   {0x148C3001, "Radeon HD 6250"},
   {0x148C3002, "Radeon HD 6290"},
   {0x148C3003, "Radeon HD 6230"},
   {0x148C3004, "Radeon HD 6350"},
   {0x148C5220, "Radeon R5 220"},
   {0x148C7350, "Radeon HD 7350"},
   {0x148C8350, "Radeon HD 8350"},
   {0x15457350, "Radeon HD 7350"},
   {0x16423987, "Radeon HD 6350"},
   {0x16423B31, "Radeon HD 6350A"},
   {0x16823270, "Radeon HD 7350"},
   {0x16823510, "Radeon HD 8350"},
   {0x16825220, "Radeon R5 220"},
   {0x174B3987, "Radeon HD 6350"},
   {0x174B5470, "Radeon HD 5470"},
   {0x174B5490, "Radeon HD 5490"},
   {0x174B5530, "Radeon HD 5530"},
   {0x174B6230, "Radeon HD 6230"},
   {0x174B6350, "Radeon HD 6350"},
   {0x174B7350, "Radeon HD 7350"},
   {0x174B8350, "Radeon HD 8350"},
   {0x174BA005, "Radeon R5 220"},
   {0x17873000, "Radeon HD 5470"},
   {0x17873001, "Radeon HD 5530"},
   {0x17873002, "Radeon HD 5490"},
   {0x17875220, "Radeon R5 220"},
   {0x17AF3000, "Radeon HD 6250"},
   {0x17AF3001, "Radeon HD 6230"},
   {0x17AF3002, "Radeon HD 6290"},
   {0x17AF3011, "Radeon HD 5470"},
   {0x17AF3012, "Radeon HD 5490"},
   {0x17AF3013, "Radeon HD 5470"},
   {0x17AF3014, "Radeon HD 6350"},
   /* 174B:3000, 174B:6250, 174B:6290 have their subsystem IDs specified incorrectly in the Windows
    * driver 15.301.1901.0000 (the subsystem vendor and device IDs are swapped).
    */
   {0x174B3000, "Radeon HD 6230"},
   {0x174B6250, "Radeon HD 6250"},
   {0x174B6290, "Radeon HD 6290"},
   {},
};

static const struct terascale_device_name_subsystem terascale_device_name_subsystems_0x68FA[] = {
   {0x103C2B32, "Radeon R5 220"},
   {0x103C2B33, "Radeon R5 220"},
   {0x14622128, "Radeon HD 7350"},
   {0x16823510, "Radeon HD 8350"},
   {0x174B3521, "Radeon R5 220"},
   {0x174B3522, "Radeon R5 220"},
   {0x174B7350, "Radeon HD 7350"},
   {0x17AF3015, "Radeon HD 7350"},
   {},
};

/* clang-format on */

static const struct terascale_device_name_device terascale_device_name_devices_0x6880[] = {
   /* Cypress: 0x6880...
    * Not using "Radeon HD 5800 Series" unlike in the Windows driver 15.301.1901.0000 because the
    * third digit is expected to correspond to XT/PRO/LE for higher-end graphics cards.
    */

   [0x00] = {{"Lexington XT GL"}},
   [0x08] = {{"Cypress XT GL", "FirePro V8800"}},
   [0x09] = {{"Cypress PRO GL", "FirePro V7800"}, terascale_device_name_subsystems_0x6889},
   [0x0A] = {{"Cypress XT GL", "FirePro V9800"}, terascale_device_name_subsystems_0x688A},
   [0x0C] = {{"Cypress FS Osprey", "FireStream 9370"}},
   [0x0D] = {{"Cypress FS Kestrel", "FireStream 9350"}},
   [0x10] = {{"Lexington XT"}},
   [0x18] = {{"Cypress XT", "Radeon HD 5870"}, terascale_device_name_subsystems_0x6898},
   [0x19] = {{"Cypress PRO", "Radeon HD 5850"}, terascale_device_name_subsystems_0x6899},
   [0x1B] = {{"Cypress PRO Rebranch", "Radeon HD 6850"}},
   [0x1C] = {{"Hemlock XT", "Radeon HD 5970"}, terascale_device_name_subsystems_0x689C},
   /* Unreleased HD 5950 (HD 5850 X2). */
   [0x1D] = {{"Hemlock PRO"}},
   [0x1E] = {{"Cypress LE", "Radeon HD 5830"}},

   /* Juniper: 0x68A0...
    * Not using "Mobility Radeon HD 5800 Series" and "Radeon HD 5700 Series" unlike in the Windows
    * driver 15.301.1901.0000 because the third digit is expected to correspond to XT/PRO/LP for
    * these graphics cards.
    */

   [0x20] = {{"Broadway XT", "Mobility Radeon HD 5870"}},
   [0x21] = {{"Broadway PRO/LP", "Mobility Radeon HD 5830/5850"}},
   [0x28] = {{"Broadway PRO/LP", "Radeon HD 6800M Series"}},
   [0x29] = {{"Juniper PRO GL", "FirePro V5800"}},
   [0x30] = {{"Broadway XT"}},
   [0x31] = {{"Broadway PRO/LP"}},
   [0x38] = {{"Juniper XT", "Radeon HD 5770"}, terascale_device_name_subsystems_0x68B8},
   /* Possibly Radeon HD 5670 640SP Edition, but not clear. Using the name from the Windows driver
    * 15.301.1901.0000 (it doesn't have "Series" there as well).
    */
   [0x39] = {{"Juniper PRO", "Radeon HD 5600/5700"}},
   [0x3A] = {{"Juniper XT BD3D", "Radeon HD 6770"}},
   [0x3E] = {{"Juniper LE", "Radeon HD 5750"}, terascale_device_name_subsystems_0x68BE},
   [0x3F] = {{"Juniper LE BD3D", "Radeon HD 6750"}, terascale_device_name_subsystems_0x68BF},

   /* Redwood: 0x68C0... */

   [0x40] = {{"Madison XT", "Mobility Radeon HD 5700 Series"}},
   [0x41] = {{"Madison PRO/LP", "Mobility Radeon HD 5650"}},
   [0x47] = {{"Pinewood", "Mobility Radeon HD 5570"}, terascale_device_name_subsystems_0x68C7},
   [0x48] = {{"Redwood XT GL", "FirePro V4800"}},
   [0x49] = {{"Redwood PRO GL", "FirePro V3800"}},
   [0x50] = {{"Madison XT"}},
   [0x51] = {{"Madison PRO/LP"}},
   [0x58] = {{"Redwood XT", "Radeon HD 5670"}, terascale_device_name_subsystems_0x68D8},
   [0x59] = {{"Redwood PRO", "Radeon HD 5570"}, terascale_device_name_subsystems_0x68D9},
   [0x5A] = {{"Redwood PRO2", "Radeon HD 5500 Series"}, terascale_device_name_subsystems_0x68DA},
   [0x5E] = {{"Redwood LE"}},

   /* Cedar: 0x68E0...
    * Not using "Mobility Radeon HD 5400 Series" unlike in the Windows driver 15.301.1901.0000
    * because the third digit is expected to correspond to XT/PRO/LE for these graphics cards.
    */

   [0x60] = {{"Park XT/PRO", "Mobility Radeon HD 5450/5470"},
             terascale_device_name_subsystems_0x68E0},
   [0x61] = {{"Park LP", "Mobility Radeon HD 5430"}, terascale_device_name_subsystems_0x68E1},
   [0x64] = {{"Robson CE", "Radeon HD 6300M Series"}, terascale_device_name_subsystems_0x68E4},
   [0x65] = {{"Robson LE", "Radeon HD 6300M Series"}, terascale_device_name_subsystems_0x68E5},
   [0x68] = {{"Cedar XT GL"}},
   [0x69] = {{"Cedar PRO GL"}},
   [0x70] = {{"Park PRO/XT"}},
   [0x71] = {{"Park LP GL", "FirePro 2460"}},
   [0x72] = {{"Cedar Prowl", "FirePro 2270"}},
   [0x78] = {{"Cedar XT"}},
   [0x79] = {{"Cedar PRO", "Radeon HD 5450"}, terascale_device_name_subsystems_0x68F9},
   /* "Radeon HD 7300 Series" in the Windows driver 15.301.1901.0000, but HD 7350 appears to be the
    * only graphics card in the HD 73xx series.
    */
   [0x7A] = {{"Cedar PRO2", "Radeon HD 7350"}, terascale_device_name_subsystems_0x68FA},
   [0x7E] = {{"Cedar LE"}},
};

/* SuperSumo:       4 quad pipes, 3-5 SIMDs and texture pipes, 2 render backends.
 * Sumo:            4 quad pipes,   2 SIMDs and texture pipes, 1 render backend.
 * Wrestler:        2 quad pipes,   2 SIMDs and texture pipes, 1 render backend.
 * Devastator:      4 quad pipes,   6 SIMDs and texture pipes, 2 render backends.
 * Devastator Lite: 4 quad pipes,   4 SIMDs and texture pipes, 2 render backends.
 * Scrapper:        4 quad pipes,   3 SIMDs and texture pipes, 1 render backend.
 * Scrapper Lite:   4 quad pipes,   2 SIMDs and texture pipes, 1 render backend.
 */

static const struct terascale_device_name_device terascale_device_name_devices_0x9640[] = {
   [0x0] = {{"SuperSumo", "Radeon HD 6550D"}},
   [0x1] = {{"SuperSumo", "Radeon HD 6620G"}},
   [0x2] = {{"Sumo", "Radeon HD 6370D"}},
   [0x3] = {{"Sumo", "Radeon HD 6380G"}},
   [0x4] = {{"Sumo", "Radeon HD 6410D"}},
   [0x5] = {{"Sumo", "Radeon HD 6410D"}},
   [0x7] = {{"SuperSumo", "Radeon HD 6520G"}},
   /* Same product name used for both chips according to the Windows driver 13.150.0.0000. */
   [0x8] = {{"SuperSumo", "Radeon HD 6480G"}},
   [0x9] = {{"Sumo", "Radeon HD 6480G"}},
   [0xA] = {{"SuperSumo", "Radeon HD 6530D"}},
   [0xB] = {{"SuperSumo"}},
   [0xC] = {{"SuperSumo"}},
   [0xE] = {{"SuperSumo"}},
   [0xF] = {{"SuperSumo"}},
};

static const struct terascale_device_name_device terascale_device_name_devices_0x9802[] = {
   {{"Wrestler", "Radeon HD 6310"}}, /* 0x9802 */
   {{"Wrestler", "Radeon HD 6310"}}, /* 0x9803 */
   {{"Wrestler", "Radeon HD 6250"}}, /* 0x9804 */
   {{"Wrestler", "Radeon HD 6250"}}, /* 0x9805 */
   {{"Wrestler", "Radeon HD 6320"}}, /* 0x9806 */
   {{"Wrestler", "Radeon HD 6290"}}, /* 0x9807 */
   {{"Wrestler", "Radeon HD 7340"}}, /* 0x9808 */
   {{"Wrestler", "Radeon HD 7310"}}, /* 0x9809 */
   {{"Wrestler", "Radeon HD 7290"}}, /* 0x980A */
};

static const struct terascale_device_name_device terascale_device_name_devices_0x9900[] = {
   [0x00] = {{"Devastator", "Radeon HD 7660G"}},
   [0x01] = {{"Devastator", "Radeon HD 7660D"}},
   [0x03] = {{"Devastator Lite", "Radeon HD 7640G"}},
   [0x04] = {{"Devastator Lite", "Radeon HD 7560D"}},
   [0x05] = {{"Devastator", "FirePro A300"}},
   [0x06] = {{"Devastator", "FirePro A320"}},
   [0x07] = {{"Devastator", "Radeon HD 7620G"}},
   [0x08] = {{"Devastator", "Radeon HD 7600G"}},
   /* 0x9909 is like 0x9908, but 17W rather than 19W, not supported by the 15.301.1901.0000 Windows
    * driver. However, 0x990X names match 0x991X names for existing devices, and 0x9919 is 7500G in
    * the 13.150.0.0000 driver. Though 0x9909 uses a non-Lite chip configuration, unlike 0x9919.
    */
   [0x09] = {{"Devastator", "Radeon HD 7500G"}},
   [0x0A] = {{"Devastator Lite", "Radeon HD 7500G"}},
   [0x0B] = {{"Devastator", "Radeon HD 8650G"}},
   [0x0C] = {{"Devastator", "Radeon HD 8670D"}},
   [0x0D] = {{"Devastator Lite", "Radeon HD 8550G"}},
   [0x0E] = {{"Devastator Lite", "Radeon HD 8570D"}},
   [0x0F] = {{"Devastator", "Radeon HD 8610G"}},
   [0x10] = {{"Devastator", "Radeon HD 7660G"}},
   [0x13] = {{"Devastator Lite", "Radeon HD 7640G"}},
   [0x17] = {{"Devastator", "Radeon HD 7620G"}},
   [0x18] = {{"Devastator", "Radeon HD 7600G"}},
   [0x19] = {{"Devastator Lite", "Radeon HD 7500G"}},
};

static const struct terascale_device_name_device terascale_device_name_devices_0x9990[] = {
   [0x00] = {{"Scrapper", "Radeon HD 7520G"}},
   [0x01] = {{"Scrapper", "Radeon HD 7540D"}},
   [0x02] = {{"Scrapper Lite", "Radeon HD 7420G"}},
   [0x03] = {{"Scrapper Lite", "Radeon HD 7480D"}},
   [0x04] = {{"Scrapper", "Radeon HD 7400G"}},
   [0x05] = {{"Scrapper", "Radeon HD 8450G"}},
   [0x06] = {{"Scrapper", "Radeon HD 8470D"}},
   [0x07] = {{"Scrapper Lite", "Radeon HD 8350G"}},
   [0x08] = {{"Scrapper Lite", "Radeon HD 8370D"}},
   [0x09] = {{"Devastator", "Radeon HD 8510G"}},
   [0x0A] = {{"Scrapper", "Radeon HD 8410G"}},
   [0x0B] = {{"Scrapper Lite", "Radeon HD 8310G"}},
   [0x0C] = {{"Devastator", "Radeon HD 8650D"}},
   [0x0D] = {{"Devastator Lite", "Radeon HD 8550D"}},
   [0x10] = {{"Scrapper", "Radeon HD 7520G"}},
   [0x12] = {{"Scrapper Lite", "Radeon HD 7420G"}},
   [0x14] = {{"Scrapper", "Radeon HD 7400G"}},
};

struct terascale_device_name_range {
   uint16_t first_device_id;
   uint16_t device_count;
   const struct terascale_device_name_device *devices;
};

#define TERASCALE_DEVICE_NAME_RANGE(first_device_id)                                               \
   {                                                                                               \
      first_device_id, ARRAY_SIZE(terascale_device_name_devices_##first_device_id),                \
         terascale_device_name_devices_##first_device_id                                           \
   }

static const struct terascale_device_name_range terascale_device_name_ranges[] = {
   TERASCALE_DEVICE_NAME_RANGE(0x6700), TERASCALE_DEVICE_NAME_RANGE(0x6840),
   TERASCALE_DEVICE_NAME_RANGE(0x6880), TERASCALE_DEVICE_NAME_RANGE(0x9640),
   TERASCALE_DEVICE_NAME_RANGE(0x9802), TERASCALE_DEVICE_NAME_RANGE(0x9900),
   TERASCALE_DEVICE_NAME_RANGE(0x9990),
};

struct terascale_device_name
terascale_device_name_get(const uint16_t device_id, const uint16_t subsystem_vendor_id,
                          const uint16_t subsystem_device_id)
{
   const uint32_t subsystem_id = ((uint32_t)subsystem_vendor_id << 16) | subsystem_device_id;

   for (size_t range_index = 0; range_index < ARRAY_SIZE(terascale_device_name_ranges);
        ++range_index) {
      const struct terascale_device_name_range *const range =
         &terascale_device_name_ranges[range_index];
      if (device_id < range->first_device_id) {
         continue;
      }
      const uint16_t range_device_index = device_id - range->first_device_id;
      if (range_device_index >= range->device_count) {
         continue;
      }

      const struct terascale_device_name_device *const device = &range->devices[range_device_index];

      {
         const struct terascale_device_name_subsystem *subsystem = device->subsystems;
         if (subsystem != NULL) {
            for (; subsystem->product_name != NULL; ++subsystem) {
               if (subsystem->id == subsystem_id) {
                  return (struct terascale_device_name){.chip = device->generic_name.chip,
                                                        .product = subsystem->product_name};
               }
            }
         }
      }

      return device->generic_name;
   }

   return (struct terascale_device_name){};
}
