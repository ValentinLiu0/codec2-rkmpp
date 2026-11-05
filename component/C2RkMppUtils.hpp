#ifndef C2_RK_MPP_UTILS_HPP
#define C2_RK_MPP_UTILS_HPP

#include <cstddef>
#include <cstdint>

#include <system/graphics.h>

#include <C2Buffer.h>
#include <C2Config.h>

#include <mpp/mpp_err.h>
#include <mpp/mpp_frame.h>
#include <mpp/rk_mpi.h>
#include <mpp/rk_type.h>

#define C2_ALIGN(x, a)              (((x)+(a)-1)&~((a)-1))
#define C2_IS_ALIGNED(x, a)         (!((x) & ((a)-1)))

namespace codec2::rkmpp {

inline const char *toStr_Format(uint32_t fmt)
{
	switch (fmt) {
		case MPP_FMT_YUV420SP:          return "YUV420SP";
		case MPP_FMT_YUV420SP_10BIT:    return "YUV420SP_10BIT";
		case MPP_FMT_RGB888:            return "RGB888";
		case MPP_FMT_RGBA8888:          return "RGBA8888";
		case MPP_FMT_BGRA8888:          return "BGRA8888";
		default:                        return "unknown";
	}
}

inline const char *toStr_Coding(uint32_t coding)
{
	switch (coding) {
		case MPP_VIDEO_CodingUnused:    return "UNUSED";
		case MPP_VIDEO_CodingMPEG2:     return "MPEG2";
		case MPP_VIDEO_CodingH263:      return "H263";
		case MPP_VIDEO_CodingMPEG4:     return "MPEG4";
		case MPP_VIDEO_CodingWMV:       return "WMV";
		case MPP_VIDEO_CodingAVC:       return "AVC";
		case MPP_VIDEO_CodingVP8:       return "VP8";
		case MPP_VIDEO_CodingVP9:       return "VP9";
		case MPP_VIDEO_CodingVC1:       return "VC1";
		case MPP_VIDEO_CodingHEVC:      return "HEVC";
		case MPP_VIDEO_CodingAVS:       return "AVS";
		case MPP_VIDEO_CodingAVS2:      return "AVS2";
		case MPP_VIDEO_CodingAV1:       return "AV1";
		default:                        return "unknown";
	}
}

inline const char *toStr_BitrateMode(uint32_t i)
{
	switch (i) {
		case MPP_ENC_RC_MODE_FIXQP: return "FIXQP";
		case MPP_ENC_RC_MODE_VBR:   return "VBR";
		case MPP_ENC_RC_MODE_CBR:   return "CBR";
		default:                    return "unknown";
	}
}



/*
 * 本项目只支持这一种 MPP 输出/输入格式。
 *
 * Rockchip 的实现里还有 planar、422、444 以及 FBC(AFBC/RFBC) 变体，本项目一律
 * 不做：那些格式要么对应本 AOSP 树里根本不存在的 RK 私有 HAL 像素格式，要么需要
 * 帧缓冲压缩的下游支持。10bit(P010) 同理。
 */
constexpr MppFrameFormat kMppFrameFormat = MPP_FMT_YUV420SP;

/*
 * 对外声明的 HAL 像素格式。
 *
 * 注意平台 gralloc(gralloc-generic) 的 YCBCR_420_888 在 mapper 侧被描述成三平面
 * planar(I420)，而 MPP 输入/输出是 NV12 半平面，两者布局不同。因此两个方向都要
 * 显式做一次布局转换，见 C2RkMppUtils.cpp 里的 copyNv12ToGraphicView() 与
 * copyGraphicViewToNv12()。
 */
constexpr int kHalPixelFormat = HAL_PIXEL_FORMAT_YCBCR_420_888;

static inline uint32_t rkAlignUp(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

/* MPP 一帧 NV12 占用的字节数（含行对齐 padding）。 */
static inline size_t nv12FrameSize(uint32_t horStride, uint32_t verStride)
{
	return (size_t)horStride * verStride * 3 / 2;
}

/*
 * Android 媒体类型 -> MPP 编码格式。
 *
 * 组件和组件仓库都要用这个映射，所以放在这里而不是各自的 .cpp 里。
 *
 * \return 不认识的媒体类型返回 MPP_VIDEO_CodingUnused。
 */
MppCodingType mppCodingFromMime(const C2String &mime);

/*
 * 把一块 NV12 数据按目标 C2GraphicView 自己声明的平面布局写进去。
 *
 * 之所以查布局而不是硬编码格式：这样无论 gralloc 把 YCBCR_420_888 描述成三平面
 * planar 还是两平面半平面，都能写对。将来 gralloc 侧改好 NV12 映射，这里不用动。
 *
 * \return C2_OK 表示写入成功；C2_BAD_VALUE 表示 view 不是 U/V 子采样的 YUV 420。
 */
c2_status_t copyNv12ToGraphicView(const uint8_t *srcY, uint32_t srcYStride,
				  const uint8_t *srcUV, uint32_t srcUvStride,
				  C2GraphicView &view,
				  uint32_t width, uint32_t height);

/*
 * copyNv12ToGraphicView() 的反方向：按源 view 自己声明的布局读出数据，写成 NV12。
 */
c2_status_t copyGraphicViewToNv12(const C2GraphicView &view,
				  uint8_t *dstY, uint32_t dstYStride,
				  uint8_t *dstUV, uint32_t dstUvStride,
				  uint32_t width, uint32_t height);

} // namespace codec2::rkmpp

#endif /* C2_RK_MPP_UTILS_HPP */
