/*
 * SPDX-FileCopyrightText: Copyright 2026 Valentin Liu (LIU, YUANCHEN) <valentinliu@icloud.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VENDOR_ROCKCHIP_H
#define VENDOR_ROCKCHIP_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/**
 * This version is the header file version. 
 * MUST be updated if any changes applied to this header.
 */
#define HEADER_VERSION_MAJOR 0
#define HEADER_VERSION_MINOR 3
#define HEADER_VERSION_PATCH 0



#if __has_include(<rk_type.h>)
#include <rk_type.h>
#else
#warning "The header file rk_type.h is not found, use the pre-defined values."
typedef int MppCodingType;

#define MPP_VIDEO_CodingUnused 0		/**< Value when coding is N/A */
#define MPP_VIDEO_CodingAutoDetect 1		/**< Autodetection of coding type */
#define MPP_VIDEO_CodingMPEG2 2			/**< AKA: H.262 */
#define MPP_VIDEO_CodingH263 3			/**< H.263 */
#define MPP_VIDEO_CodingMPEG4 4			/**< MPEG-4 */
#define MPP_VIDEO_CodingWMV 5			/**< Windows Media Video (WMV1,WMV2,WMV3)*/
#define MPP_VIDEO_CodingRV 6			/**< all versions of Real Video */
#define MPP_VIDEO_CodingAVC 7			/**< H.264/AVC */
#define MPP_VIDEO_CodingMJPEG 8			/**< Motion JPEG */
#define MPP_VIDEO_CodingVP8 9			/**< VP8 */
#define MPP_VIDEO_CodingVP9 10			/**< VP9 */
#define MPP_VIDEO_CodingVC1 0x01000000		/**< Windows Media Video (WMV1,WMV2,WMV3)*/
#define MPP_VIDEO_CodingFLV1 0x01000001		/**< Sorenson H.263 */
#define MPP_VIDEO_CodingDIVX3 0x01000002	/**< DIVX3 */
#define MPP_VIDEO_CodingVP6 0x01000003
#define MPP_VIDEO_CodingHEVC 0x01000004		/**< H.265/HEVC */
#define MPP_VIDEO_CodingAVSPLUS 0x01000005	/**< AVS+ */
#define MPP_VIDEO_CodingAVS 0x01000006		/**< AVS profile=0x20 */
#define MPP_VIDEO_CodingAVS2 0x01000007		/**< AVS2 */
#define MPP_VIDEO_CodingAV1 0x01000008		/**< av1 */
#define MPP_VIDEO_CodingKhronosExtensions 0x6F000000	/**< Reserved region for introducing Khronos Standard Extensions */
#define MPP_VIDEO_CodingVendorStartUnused 0x7F000000	/**< Reserved region for introducing Vendor Extensions */
#define MPP_VIDEO_CodingMax 0x7FFFFFFF

#endif

/* === Rockchip Gralloc === */
/* Android HAL format */
#define HAL_PIXEL_FORMAT_YCrCb_NV12 0x15
#define HAL_PIXEL_FORMAT_YCrCb_NV12_10 0x17

/* Gralloc Usage */
#define RK_GRALLOC_USAGE_RKVDEC_SCALING 0x1000000



/* === Rockchip Codec2 === */
/* Chip type */
#define RK_CHIP_UNKOWN 0
// 2928 and 3036 no iep
#define RK_CHIP_2928 1
#define RK_CHIP_3036 2
#define RK_CHIP_3066 3
#define RK_CHIP_3188 4
// iep
#define RK_CHIP_3368H 5
#define RK_CHIP_3368A 6
#define RK_CHIP_3128H 7
#define RK_CHIP_3128M 8
#define RK_CHIP_312X 9
#define RK_CHIP_3326 10
// support 10bit chips
#define RK_CHIP_10BIT_SUPPORT_BEGIN 11
// 3288 support max width to 3840
#define RK_CHIP_3288 12
// support 4k chips
#define RK_CHIP_4K_SUPPORT_BEGIN 13
#define RK_CHIP_322X_SUPPORT_BEGIN 14
#define RK_CHIP_3228A 15
#define RK_CHIP_3228B 16
#define RK_CHIP_3228H 17
#define RK_CHIP_3328 18
#define RK_CHIP_3229 19
#define RK_CHIP_322X_SUPPORT_END 20
#define RK_CHIP_3399 21
#define RK_CHIP_1126 22
#define RK_CHIP_3562 23
// support 8k chips
#define RK_CHIP_8K_SUPPORT_BEGIN 24
#define RK_CHIP_356X 25
#define RK_CHIP_3528 26
#define RK_CHIP_3576 27
#define RK_CHIP_3588 28
#define RK_CHIP_8K_SUPPORT_END 29
#define RK_CHIP_10BIT_SUPPORT_END 30
#define RK_CHIP_3368 31
#define RK_CHIP_4K_SUPPORT_END 32

/* Capability 10-bit */
// unsupport 10bit
#define RK_C2_CAP_10BIT_NONE   0
#define RK_C2_CAP_10BIT_AVC    0x1
#define RK_C2_CAP_10BIT_HEVC   0x2
#define RK_C2_CAP_10BIT_VP9    0x4

/* Scale mode */
#define RK_C2_SCALE_MODE_NONE          0
#define RK_C2_SCALE_MODE_META          1
#define RK_C2_SCALE_MODE_DOWN_SCALE    2

typedef struct rk_chip_info {
	int		chip_type;
	const char	*chip_name;
	uint32_t	scale_mode;
	uint32_t	cap_10bit;
	uint32_t	cap_hdr_meta;
	uint32_t	reserved;
} rk_chip_info_t;

static const rk_chip_info rk_chip_infos[] = {
	{
		RK_CHIP_3588,
		"rk3588",
		RK_C2_SCALE_MODE_NONE,
		RK_C2_CAP_10BIT_AVC | RK_C2_CAP_10BIT_HEVC | RK_C2_CAP_10BIT_VP9,
		0,
		0,
	},
	{
		RK_CHIP_UNKOWN,
		"unknown",
		RK_C2_SCALE_MODE_NONE,
		RK_C2_CAP_10BIT_NONE,
		0,
		0,
	}
};
static const size_t rk_chip_infos_num = sizeof(rk_chip_infos) / sizeof(rk_chip_infos[0]);

#define CHIP_NAME_MAX 8
inline char *rk_chip_get_name(void)
{
	char buf[128];
	const char *comma, *p, *dash;
	ssize_t n;
	size_t len;
	int fd;

	fd = open("/proc/device-tree/compatible", O_RDONLY);
	if (fd < 0)
		return nullptr;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return nullptr;
	buf[n] = '\0';

	/* First entry is the board, like "rockchip,rk3588-blueberry-edge-v12" */
	comma = strchr(buf, ',');
	if (comma == NULL)
		return nullptr;
	p = comma + 1;

	/* The chip name ends at the first '-' (or at the end of the entry). */
	dash = strchr(p, '-');
	len = dash != NULL ? (size_t)(dash - p) : strlen(p);
	if (len == 0 || len >= CHIP_NAME_MAX)
		return nullptr;

	char out[CHIP_NAME_MAX];
	memcpy(out, p, len);
	out[len] = '\0';
	return strdup(out);
}

inline int rk_chip_get_type(void)
{
	int chip_type = RK_CHIP_UNKOWN;
	char *chip_name = rk_chip_get_name();

	if (chip_name == nullptr) {
		return RK_CHIP_UNKOWN;
	}

	if (!strcmp("rk3588", chip_name)) {
		chip_type = RK_CHIP_3588;
	} else if (!strcmp("rk3576", chip_name)) {
		chip_type = RK_CHIP_3576;
	} else if (!strcmp("rk3399", chip_name)) {
		chip_type = RK_CHIP_3399;
	}

	free(chip_name);
	return chip_type;
}

#endif /* VENDOR_ROCKCHIP_H */
