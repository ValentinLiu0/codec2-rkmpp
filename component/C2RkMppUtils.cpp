//#define LOG_NDEBUG 0
#define LOG_TAG "C2RkMppUtils"
#include <log/log.h>

#include <cstring>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2RkMppUtils.hpp>

namespace codec2::rkmpp {

MppCodingType mppCodingFromMime(const C2String &mime)
{
	if (mime == android::MEDIA_MIMETYPE_VIDEO_AVC) {
		return MPP_VIDEO_CodingAVC;
	}
	if (mime == android::MEDIA_MIMETYPE_VIDEO_HEVC) {
		return MPP_VIDEO_CodingHEVC;
	}
	if (mime == android::MEDIA_MIMETYPE_VIDEO_VP8) {
		return MPP_VIDEO_CodingVP8;
	}
	if (mime == android::MEDIA_MIMETYPE_VIDEO_VP9) {
		return MPP_VIDEO_CodingVP9;
	}
	if (mime == android::MEDIA_MIMETYPE_VIDEO_AV1) {
		return MPP_VIDEO_CodingAV1;
	}
	if (mime == android::MEDIA_MIMETYPE_VIDEO_MPEG2) {
		return MPP_VIDEO_CodingMPEG2;
	}
	if (mime == android::MEDIA_MIMETYPE_VIDEO_MPEG4) {
		return MPP_VIDEO_CodingMPEG4;
	}
	if (mime == android::MEDIA_MIMETYPE_VIDEO_H263) {
		return MPP_VIDEO_CodingH263;
	}
	return MPP_VIDEO_CodingUnused;
}

/*
 * 下面的读写都按 C2GraphicView 自己声明的平面布局走，而不是假设某个固定格式。
 * 这样 gralloc 把 YCBCR_420_888 描述成三平面 planar(I420) 还是两平面半平面(NV12)
 * 都能正确工作，将来 gralloc 侧的格式映射改动了这里也不需要跟着改。
 *
 * 半平面与三平面的唯一区别体现在色度两个分量平面的 colInc 上：
 *   planar  : colInc(U) == 1, colInc(V) == 1
 *   半平面  : colInc(U) == 2, colInc(V) == 2，且 V 的 rootIx 指向 U
 * 所以用 colInc 做乘子就能同时覆盖两种情况。
 */

static bool isSupportedYuv420(const C2PlanarLayout &layout)
{
	if (layout.type != C2PlanarLayout::TYPE_YUV || layout.rootPlanes == 0) {
		return false;
	}
	const C2PlaneInfo &planeU = layout.planes[C2PlanarLayout::PLANE_U];
	const C2PlaneInfo &planeV = layout.planes[C2PlanarLayout::PLANE_V];
	return planeU.colSampling == 2 && planeU.rowSampling == 2 &&
	       planeV.colSampling == 2 && planeV.rowSampling == 2;
}

c2_status_t copyNv12ToGraphicView(const uint8_t *srcY, uint32_t srcYStride,
				  const uint8_t *srcUV, uint32_t srcUvStride,
				  C2GraphicView &view,
				  uint32_t width, uint32_t height)
{
	if (srcY == nullptr || srcUV == nullptr || view.data() == nullptr) {
		return C2_BAD_VALUE;
	}
	if (width == 0 || height == 0 ||
	    width > view.width() || height > view.height()) {
		ALOGE("bad copy size %ux%u into %ux%u", width, height, view.width(), view.height());
		return C2_BAD_VALUE;
	}

	const C2PlanarLayout layout = view.layout();
	if (!isSupportedYuv420(layout)) {
		ALOGE("unsupported output layout type %u rootPlanes %u",
		      layout.type, layout.rootPlanes);
		return C2_BAD_VALUE;
	}

	const C2PlaneInfo &planeY = layout.planes[C2PlanarLayout::PLANE_Y];
	const C2PlaneInfo &planeU = layout.planes[C2PlanarLayout::PLANE_U];
	const C2PlaneInfo &planeV = layout.planes[C2PlanarLayout::PLANE_V];

	intptr_t dstY = (intptr_t)view.data()[C2PlanarLayout::PLANE_Y];
	intptr_t dstU = (intptr_t)view.data()[C2PlanarLayout::PLANE_U];
	intptr_t dstV = (intptr_t)view.data()[C2PlanarLayout::PLANE_V];

	/* 亮度平面 */
	for (uint32_t y = 0; y < height; ++y) {
		uint8_t *row = (uint8_t *)(dstY + (intptr_t)y * planeY.rowInc);
		if (planeY.colInc == 1) {
			memcpy(row, srcY + (size_t)y * srcYStride, width);
		} else {
			const uint8_t *src = srcY + (size_t)y * srcYStride;
			for (uint32_t x = 0; x < width; ++x) {
				row[(intptr_t)x * planeY.colInc] = src[x];
			}
		}
	}

	/* 色度平面：NV12 是交织的 UV，按 colInc 展开即可落到 planar 或半平面 */
	for (uint32_t y = 0; y < height / 2; ++y) {
		const uint8_t *src = srcUV + (size_t)y * srcUvStride;
		uint8_t *rowU = (uint8_t *)(dstU + (intptr_t)y * planeU.rowInc);
		uint8_t *rowV = (uint8_t *)(dstV + (intptr_t)y * planeV.rowInc);
		for (uint32_t x = 0; x < width / 2; ++x) {
			rowU[(intptr_t)x * planeU.colInc] = src[2 * x];
			rowV[(intptr_t)x * planeV.colInc] = src[2 * x + 1];
		}
	}

	return C2_OK;
}

c2_status_t copyGraphicViewToNv12(const C2GraphicView &view,
				  uint8_t *dstY, uint32_t dstYStride,
				  uint8_t *dstUV, uint32_t dstUvStride,
				  uint32_t width, uint32_t height)
{
	if (dstY == nullptr || dstUV == nullptr || view.data() == nullptr) {
		return C2_BAD_VALUE;
	}
	if (width == 0 || height == 0 ||
	    width > view.width() || height > view.height()) {
		ALOGE("bad copy size %ux%u from %ux%u", width, height, view.width(), view.height());
		return C2_BAD_VALUE;
	}

	const C2PlanarLayout layout = view.layout();
	if (!isSupportedYuv420(layout)) {
		ALOGE("unsupported input layout type %u rootPlanes %u",
		      layout.type, layout.rootPlanes);
		return C2_BAD_VALUE;
	}

	const C2PlaneInfo &planeY = layout.planes[C2PlanarLayout::PLANE_Y];
	const C2PlaneInfo &planeU = layout.planes[C2PlanarLayout::PLANE_U];
	const C2PlaneInfo &planeV = layout.planes[C2PlanarLayout::PLANE_V];

	intptr_t srcY = (intptr_t)view.data()[C2PlanarLayout::PLANE_Y];
	intptr_t srcU = (intptr_t)view.data()[C2PlanarLayout::PLANE_U];
	intptr_t srcV = (intptr_t)view.data()[C2PlanarLayout::PLANE_V];

	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *row = (const uint8_t *)(srcY + (intptr_t)y * planeY.rowInc);
		if (planeY.colInc == 1) {
			memcpy(dstY + (size_t)y * dstYStride, row, width);
		} else {
			uint8_t *dst = dstY + (size_t)y * dstYStride;
			for (uint32_t x = 0; x < width; ++x) {
				dst[x] = row[(intptr_t)x * planeY.colInc];
			}
		}
	}

	for (uint32_t y = 0; y < height / 2; ++y) {
		uint8_t *dst = dstUV + (size_t)y * dstUvStride;
		const uint8_t *rowU =
			(const uint8_t *)(srcU + (intptr_t)y * planeU.rowInc);
		const uint8_t *rowV =
			(const uint8_t *)(srcV + (intptr_t)y * planeV.rowInc);
		for (uint32_t x = 0; x < width / 2; ++x) {
			dst[2 * x]     = rowU[(intptr_t)x * planeU.colInc];
			dst[2 * x + 1] = rowV[(intptr_t)x * planeV.colInc];
		}
	}

	return C2_OK;
}

} // namespace codec2::rkmpp
