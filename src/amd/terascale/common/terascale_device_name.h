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

#ifndef TERASCALE_DEVICE_NAME_H
#define TERASCALE_DEVICE_NAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terascale_device_name {
   /* NULL if the device doesn't exist or isn't known.
    *
    * Can be used as a part of the device name reported to applications, for instance, in
    * parentheses or after a dash, so the implementation should avoid those characters.
    *
    * "GL" should be after XT/PRO/LE because non-GL (Radeon) and GL (FirePro) versions of one chip
    * generally have close performance characteristics, thus XT/PRO/LE is more descriptive.
    */
   const char *chip;

   /* NULL if not a production device.
    *
    * Usually starts with "Radeon HD xxxx", "Mobility Radeon HD xxxx", "Radeon HD xxxxM", "FirePro",
    * "FireStream", though there are exceptions.
    *
    * It is desirable to specify both the product and the chip, if both are available, in the device
    * name in the client API because multiple chips, even from different families, including
    * different generations, were released under the same product name in some cases, so that may
    * improve the quality of issue and benchmarking reports submitted in various places by users.
    */
   const char *product;
};

struct terascale_device_name terascale_device_name_get(uint16_t device_id,
                                                       uint16_t subsystem_vendor_id,
                                                       uint16_t subsystem_device_id);

#ifdef __cplusplus
}
#endif

#endif /* TERASCALE_DEVICE_NAME_H */
