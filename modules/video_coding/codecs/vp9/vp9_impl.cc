/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 */

#include "modules/video_coding/codecs/vp9/vp9_impl.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

#include "vpx/vpx_encoder.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8cx.h"
#include "vpx/vp8dx.h"

#include "common_video/include/video_frame_buffer.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/codecs/vp9/screenshare_layers.h"
#include "modules/video_coding/codecs/vp9/svc_rate_allocator.h"
#include "rtc_base/checks.h"
#include "rtc_base/keep_ref_until_done.h"
#include "rtc_base/logging.h"
#include "rtc_base/ptr_util.h"
#include "rtc_base/timeutils.h"
#include "rtc_base/trace_event.h"

namespace webrtc {

// Only positive speeds, range for real-time coding currently is: 5 - 8.
// Lower means slower/better quality, higher means fastest/lower quality.
int GetCpuSpeed(int width, int height) {
#if defined(WEBRTC_ARCH_ARM) || defined(WEBRTC_ARCH_ARM64) || defined(ANDROID)
  return 8;
#else
  // For smaller resolutions, use lower speed setting (get some coding gain at
  // the cost of increased encoding complexity).
  if (width * height <= 352 * 288)
    return 5;
  else
    return 7;
#endif
}

bool VP9Encoder::IsSupported() {
  return true;
}

std::unique_ptr<VP9Encoder> VP9Encoder::Create() {
  return rtc::MakeUnique<VP9EncoderImpl>();
}

void VP9EncoderImpl::EncoderOutputCodedPacketCallback(vpx_codec_cx_pkt* pkt,
                                                      void* user_data) {
  VP9EncoderImpl* enc = static_cast<VP9EncoderImpl*>(user_data);
  enc->GetEncodedLayerFrame(pkt);
}

VP9EncoderImpl::VP9EncoderImpl()
    : encoded_image_(),
      encoded_complete_callback_(nullptr),
      inited_(false),
      timestamp_(0),
      cpu_speed_(3),
      rc_max_intra_target_(0),
      encoder_(nullptr),
      config_(nullptr),
      raw_(nullptr),
      input_image_(nullptr),
      force_key_frame_(true),
      pics_since_key_(0),
      num_temporal_layers_(0),
      num_spatial_layers_(0),
      inter_layer_pred_(InterLayerPredMode::kOn),
      is_flexible_mode_(false),
      frames_encoded_(0),
      // Use two spatial when screensharing with flexible mode.
      spatial_layer_(new ScreenshareLayersVP9(2)) {
  memset(&codec_, 0, sizeof(codec_));
  memset(&svc_params_, 0, sizeof(vpx_svc_extra_cfg_t));
}

VP9EncoderImpl::~VP9EncoderImpl() {
  Release();
}

int VP9EncoderImpl::Release() {
  int ret_val = WEBRTC_VIDEO_CODEC_OK;

  if (encoded_image_._buffer != nullptr) {
    delete[] encoded_image_._buffer;
    encoded_image_._buffer = nullptr;
  }
  if (encoder_ != nullptr) {
    if (inited_) {
      if (vpx_codec_destroy(encoder_)) {
        ret_val = WEBRTC_VIDEO_CODEC_MEMORY;
      }
    }
    delete encoder_;
    encoder_ = nullptr;
  }
  if (config_ != nullptr) {
    delete config_;
    config_ = nullptr;
  }
  if (raw_ != nullptr) {
    vpx_img_free(raw_);
    raw_ = nullptr;
  }
  inited_ = false;
  return ret_val;
}

bool VP9EncoderImpl::ExplicitlyConfiguredSpatialLayers() const {
  // We check target_bitrate_bps of the 0th layer to see if the spatial layers
  // (i.e. bitrates) were explicitly configured.
  return num_spatial_layers_ > 1 && codec_.spatialLayers[0].targetBitrate > 0;
}

bool VP9EncoderImpl::SetSvcRates(
    const VideoBitrateAllocation& bitrate_allocation) {
  uint8_t i = 0;

  config_->rc_target_bitrate = bitrate_allocation.get_sum_kbps();
  spatial_layer_->ConfigureBitrate(bitrate_allocation.get_sum_kbps(), 0);

  if (ExplicitlyConfiguredSpatialLayers()) {
    for (size_t sl_idx = 0; sl_idx < num_spatial_layers_; ++sl_idx) {
      const bool was_layer_enabled = (config_->ss_target_bitrate[sl_idx] > 0);
      config_->ss_target_bitrate[sl_idx] =
          bitrate_allocation.GetSpatialLayerSum(sl_idx) / 1000;

      for (size_t tl_idx = 0; tl_idx < num_temporal_layers_; ++tl_idx) {
        config_->layer_target_bitrate[sl_idx * num_temporal_layers_ + tl_idx] =
            bitrate_allocation.GetTemporalLayerSum(sl_idx, tl_idx) / 1000;
      }

      const bool is_layer_enabled = (config_->ss_target_bitrate[sl_idx] > 0);
      if (is_layer_enabled && !was_layer_enabled) {
        if (inter_layer_pred_ == InterLayerPredMode::kOff ||
            inter_layer_pred_ == InterLayerPredMode::kOnKeyPic) {
          // TODO(wemb:1526): remove key frame request when issue is fixed.
          force_key_frame_ = true;
        }
      }
    }
  } else {
    float rate_ratio[VPX_MAX_LAYERS] = {0};
    float total = 0;

    for (i = 0; i < num_spatial_layers_; ++i) {
      if (svc_params_.scaling_factor_num[i] <= 0 ||
          svc_params_.scaling_factor_den[i] <= 0) {
        RTC_LOG(LS_ERROR) << "Scaling factors not specified!";
        return false;
      }
      rate_ratio[i] =
          static_cast<float>(svc_params_.scaling_factor_num[i]) /
          svc_params_.scaling_factor_den[i];
      total += rate_ratio[i];
    }

    for (i = 0; i < num_spatial_layers_; ++i) {
      config_->ss_target_bitrate[i] = static_cast<unsigned int>(
          config_->rc_target_bitrate * rate_ratio[i] / total);
      if (num_temporal_layers_ == 1) {
        config_->layer_target_bitrate[i] = config_->ss_target_bitrate[i];
      } else if (num_temporal_layers_ == 2) {
        config_->layer_target_bitrate[i * num_temporal_layers_] =
            config_->ss_target_bitrate[i] * 2 / 3;
        config_->layer_target_bitrate[i * num_temporal_layers_ + 1] =
            config_->ss_target_bitrate[i];
      } else if (num_temporal_layers_ == 3) {
        config_->layer_target_bitrate[i * num_temporal_layers_] =
            config_->ss_target_bitrate[i] / 2;
        config_->layer_target_bitrate[i * num_temporal_layers_ + 1] =
            config_->layer_target_bitrate[i * num_temporal_layers_] +
            (config_->ss_target_bitrate[i] / 4);
        config_->layer_target_bitrate[i * num_temporal_layers_ + 2] =
            config_->ss_target_bitrate[i];
      } else {
        RTC_LOG(LS_ERROR) << "Unsupported number of temporal layers: "
                          << num_temporal_layers_;
        return false;
      }
    }
  }

  // For now, temporal layers only supported when having one spatial layer.
  if (num_spatial_layers_ == 1) {
    for (i = 0; i < num_temporal_layers_; ++i) {
      config_->ts_target_bitrate[i] = config_->layer_target_bitrate[i];
    }
  }

  return true;
}

int VP9EncoderImpl::SetRateAllocation(
    const VideoBitrateAllocation& bitrate_allocation,
    uint32_t frame_rate) {
  if (!inited_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (encoder_->err) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (frame_rate < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  // Update bit rate
  if (codec_.maxBitrate > 0 &&
      bitrate_allocation.get_sum_kbps() > codec_.maxBitrate) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  codec_.maxFramerate = frame_rate;

  if (!SetSvcRates(bitrate_allocation)) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  // Update encoder context
  if (vpx_codec_enc_config_set(encoder_, config_)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP9EncoderImpl::InitEncode(const VideoCodec* inst,
                               int number_of_cores,
                               size_t /*max_payload_size*/) {
  if (inst == nullptr) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->maxFramerate < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  // Allow zero to represent an unspecified maxBitRate
  if (inst->maxBitrate > 0 && inst->startBitrate > inst->maxBitrate) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->width < 1 || inst->height < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (number_of_cores < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->VP9().numberOfTemporalLayers > 3) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  // libvpx probably does not support more than 3 spatial layers.
  if (inst->VP9().numberOfSpatialLayers > 3) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  int ret_val = Release();
  if (ret_val < 0) {
    return ret_val;
  }
  if (encoder_ == nullptr) {
    encoder_ = new vpx_codec_ctx_t;
  }
  if (config_ == nullptr) {
    config_ = new vpx_codec_enc_cfg_t;
  }
  timestamp_ = 0;
  if (&codec_ != inst) {
    codec_ = *inst;
  }

  num_spatial_layers_ = inst->VP9().numberOfSpatialLayers;
  RTC_DCHECK_GT(num_spatial_layers_, 0);
  num_temporal_layers_ = inst->VP9().numberOfTemporalLayers;
  if (num_temporal_layers_ == 0)
    num_temporal_layers_ = 1;

  // Allocate memory for encoded image
  if (encoded_image_._buffer != nullptr) {
    delete[] encoded_image_._buffer;
  }
  encoded_image_._size =
      CalcBufferSize(VideoType::kI420, codec_.width, codec_.height);
  encoded_image_._buffer = new uint8_t[encoded_image_._size];
  encoded_image_._completeFrame = true;
  // Creating a wrapper to the image - setting image data to nullptr. Actual
  // pointer will be set in encode. Setting align to 1, as it is meaningless
  // (actual memory is not allocated).
  raw_ = vpx_img_wrap(nullptr, VPX_IMG_FMT_I420, codec_.width, codec_.height, 1,
                      nullptr);
  // Populate encoder configuration with default values.
  if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), config_, 0)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  config_->g_w = codec_.width;
  config_->g_h = codec_.height;
  config_->rc_target_bitrate = inst->startBitrate;  // in kbit/s
  config_->g_error_resilient =
      (num_spatial_layers_ > 1 || num_temporal_layers_ > 1) ? 1 : 0;
  // Setting the time base of the codec.
  config_->g_timebase.num = 1;
  config_->g_timebase.den = 90000;
  config_->g_lag_in_frames = 0;  // 0- no frame lagging
  config_->g_threads = 1;
  // Rate control settings.
  config_->rc_dropframe_thresh = inst->VP9().frameDroppingOn ? 30 : 0;
  config_->rc_end_usage = VPX_CBR;
  config_->g_pass = VPX_RC_ONE_PASS;
  config_->rc_min_quantizer = 2;
  config_->rc_max_quantizer = 52;
  config_->rc_undershoot_pct = 50;
  config_->rc_overshoot_pct = 50;
  config_->rc_buf_initial_sz = 500;
  config_->rc_buf_optimal_sz = 600;
  config_->rc_buf_sz = 1000;
  // Set the maximum target size of any key-frame.
  rc_max_intra_target_ = MaxIntraTarget(config_->rc_buf_optimal_sz);
  if (inst->VP9().keyFrameInterval > 0) {
    config_->kf_mode = VPX_KF_AUTO;
    config_->kf_max_dist = inst->VP9().keyFrameInterval;
    // Needs to be set (in svc mode) to get correct periodic key frame interval
    // (will have no effect in non-svc).
    config_->kf_min_dist = config_->kf_max_dist;
  } else {
    config_->kf_mode = VPX_KF_DISABLED;
  }
  config_->rc_resize_allowed = inst->VP9().automaticResizeOn ? 1 : 0;
  // Determine number of threads based on the image size and #cores.
  config_->g_threads =
      NumberOfThreads(config_->g_w, config_->g_h, number_of_cores);

  cpu_speed_ = GetCpuSpeed(config_->g_w, config_->g_h);

  // TODO(asapersson): Check configuration of temporal switch up and increase
  // pattern length.
  is_flexible_mode_ = inst->VP9().flexibleMode;
  if (is_flexible_mode_) {
    config_->temporal_layering_mode = VP9E_TEMPORAL_LAYERING_MODE_BYPASS;
    config_->ts_number_layers = num_temporal_layers_;
    if (codec_.mode == kScreensharing)
      spatial_layer_->ConfigureBitrate(inst->startBitrate, 0);
  } else if (num_temporal_layers_ == 1) {
    gof_.SetGofInfoVP9(kTemporalStructureMode1);
    config_->temporal_layering_mode = VP9E_TEMPORAL_LAYERING_MODE_NOLAYERING;
    config_->ts_number_layers = 1;
    config_->ts_rate_decimator[0] = 1;
    config_->ts_periodicity = 1;
    config_->ts_layer_id[0] = 0;
  } else if (num_temporal_layers_ == 2) {
    gof_.SetGofInfoVP9(kTemporalStructureMode2);
    config_->temporal_layering_mode = VP9E_TEMPORAL_LAYERING_MODE_0101;
    config_->ts_number_layers = 2;
    config_->ts_rate_decimator[0] = 2;
    config_->ts_rate_decimator[1] = 1;
    config_->ts_periodicity = 2;
    config_->ts_layer_id[0] = 0;
    config_->ts_layer_id[1] = 1;
  } else if (num_temporal_layers_ == 3) {
    gof_.SetGofInfoVP9(kTemporalStructureMode3);
    config_->temporal_layering_mode = VP9E_TEMPORAL_LAYERING_MODE_0212;
    config_->ts_number_layers = 3;
    config_->ts_rate_decimator[0] = 4;
    config_->ts_rate_decimator[1] = 2;
    config_->ts_rate_decimator[2] = 1;
    config_->ts_periodicity = 4;
    config_->ts_layer_id[0] = 0;
    config_->ts_layer_id[1] = 2;
    config_->ts_layer_id[2] = 1;
    config_->ts_layer_id[3] = 2;
  } else {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  inter_layer_pred_ = inst->VP9().interLayerPred;

  return InitAndSetControlSettings(inst);
}

int VP9EncoderImpl::NumberOfThreads(int width,
                                    int height,
                                    int number_of_cores) {
  // Keep the number of encoder threads equal to the possible number of column
  // tiles, which is (1, 2, 4, 8). See comments below for VP9E_SET_TILE_COLUMNS.
  if (width * height >= 1280 * 720 && number_of_cores > 4) {
    return 4;
  } else if (width * height >= 640 * 360 && number_of_cores > 2) {
    return 2;
  } else {
    // Use 2 threads for low res on ARM.
#if defined(WEBRTC_ARCH_ARM) || defined(WEBRTC_ARCH_ARM64) || \
    defined(WEBRTC_ANDROID)
    if (width * height >= 320 * 180 && number_of_cores > 2) {
      return 2;
    }
#endif
    // 1 thread less than VGA.
    return 1;
  }
}

int VP9EncoderImpl::InitAndSetControlSettings(const VideoCodec* inst) {
  // Set QP-min/max per spatial and temporal layer.
  int tot_num_layers = num_spatial_layers_ * num_temporal_layers_;
  for (int i = 0; i < tot_num_layers; ++i) {
    svc_params_.max_quantizers[i] = config_->rc_max_quantizer;
    svc_params_.min_quantizers[i] = config_->rc_min_quantizer;
  }
  config_->ss_number_layers = num_spatial_layers_;
  if (ExplicitlyConfiguredSpatialLayers()) {
    for (int i = 0; i < num_spatial_layers_; ++i) {
      const auto& layer = codec_.spatialLayers[i];
      const int scale_factor = codec_.width / layer.width;
      RTC_DCHECK_GT(scale_factor, 0);

      // Ensure scaler factor is integer.
      if (scale_factor * layer.width != codec_.width) {
        return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
      }

      // Ensure scale factor is the same in both dimensions.
      if (scale_factor * layer.height != codec_.height) {
        return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
      }

      // Ensure scale factor is power of two.
      const bool is_pow_of_two = (scale_factor & (scale_factor - 1)) == 0;
      if (!is_pow_of_two) {
        return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
      }

      svc_params_.scaling_factor_num[i] = 1;
      svc_params_.scaling_factor_den[i] = scale_factor;
    }
  } else {
    int scaling_factor_num = 256;
    for (int i = num_spatial_layers_ - 1; i >= 0; --i) {
      // 1:2 scaling in each dimension.
      svc_params_.scaling_factor_num[i] = scaling_factor_num;
      svc_params_.scaling_factor_den[i] = 256;
      if (codec_.mode != kScreensharing)
        scaling_factor_num /= 2;
    }
  }

  SvcRateAllocator init_allocator(codec_);
  VideoBitrateAllocation allocation = init_allocator.GetAllocation(
      inst->startBitrate * 1000, inst->maxFramerate);
  if (!SetSvcRates(allocation)) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if (vpx_codec_enc_init(encoder_, vpx_codec_vp9_cx(), config_, 0)) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  vpx_codec_control(encoder_, VP8E_SET_CPUUSED, cpu_speed_);
  vpx_codec_control(encoder_, VP8E_SET_MAX_INTRA_BITRATE_PCT,
                    rc_max_intra_target_);
  vpx_codec_control(encoder_, VP9E_SET_AQ_MODE,
                    inst->VP9().adaptiveQpMode ? 3 : 0);

  vpx_codec_control(encoder_, VP9E_SET_FRAME_PARALLEL_DECODING, 0);
  vpx_codec_control(
      encoder_, VP9E_SET_SVC,
      (num_temporal_layers_ > 1 || num_spatial_layers_ > 1) ? 1 : 0);

  if (num_temporal_layers_ > 1 || num_spatial_layers_ > 1) {
    vpx_codec_control(encoder_, VP9E_SET_SVC_PARAMETERS,
                      &svc_params_);
  }

  if (num_spatial_layers_ > 1) {
    switch (inter_layer_pred_) {
      case InterLayerPredMode::kOn:
        vpx_codec_control(encoder_, VP9E_SET_SVC_INTER_LAYER_PRED, 0);
        break;
      case InterLayerPredMode::kOff:
        vpx_codec_control(encoder_, VP9E_SET_SVC_INTER_LAYER_PRED, 1);
        break;
      case InterLayerPredMode::kOnKeyPic:
        vpx_codec_control(encoder_, VP9E_SET_SVC_INTER_LAYER_PRED, 2);
        break;
      default:
        RTC_NOTREACHED();
    }
  }

  // Register callback for getting each spatial layer.
  vpx_codec_priv_output_cx_pkt_cb_pair_t cbp = {
      VP9EncoderImpl::EncoderOutputCodedPacketCallback,
      reinterpret_cast<void*>(this)};
  vpx_codec_control(encoder_, VP9E_REGISTER_CX_CALLBACK,
                    reinterpret_cast<void*>(&cbp));

  // Control function to set the number of column tiles in encoding a frame, in
  // log2 unit: e.g., 0 = 1 tile column, 1 = 2 tile columns, 2 = 4 tile columns.
  // The number tile columns will be capped by the encoder based on image size
  // (minimum width of tile column is 256 pixels, maximum is 4096).
  vpx_codec_control(encoder_, VP9E_SET_TILE_COLUMNS, (config_->g_threads >> 1));

  // Turn on row-based multithreading.
  vpx_codec_control(encoder_, VP9E_SET_ROW_MT, 1);

#if !defined(WEBRTC_ARCH_ARM) && !defined(WEBRTC_ARCH_ARM64) && \
  !defined(ANDROID)
  // Do not enable the denoiser on ARM since optimization is pending.
  // Denoiser is on by default on other platforms.
  vpx_codec_control(encoder_, VP9E_SET_NOISE_SENSITIVITY,
                    inst->VP9().denoisingOn ? 1 : 0);
#endif

  if (codec_.mode == kScreensharing) {
    // Adjust internal parameters to screen content.
    vpx_codec_control(encoder_, VP9E_SET_TUNE_CONTENT, 1);
  }
  // Enable encoder skip of static/low content blocks.
  vpx_codec_control(encoder_, VP8E_SET_STATIC_THRESHOLD, 1);
  inited_ = true;
  return WEBRTC_VIDEO_CODEC_OK;
}

uint32_t VP9EncoderImpl::MaxIntraTarget(uint32_t optimal_buffer_size) {
  // Set max to the optimal buffer level (normalized by target BR),
  // and scaled by a scale_par.
  // Max target size = scale_par * optimal_buffer_size * targetBR[Kbps].
  // This value is presented in percentage of perFrameBw:
  // perFrameBw = targetBR[Kbps] * 1000 / framerate.
  // The target in % is as follows:
  float scale_par = 0.5;
  uint32_t target_pct =
      optimal_buffer_size * scale_par * codec_.maxFramerate / 10;
  // Don't go below 3 times the per frame bandwidth.
  const uint32_t min_intra_size = 300;
  return (target_pct < min_intra_size) ? min_intra_size : target_pct;
}

int VP9EncoderImpl::Encode(const VideoFrame& input_image,
                           const CodecSpecificInfo* codec_specific_info,
                           const std::vector<FrameType>* frame_types) {
  if (!inited_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (encoded_complete_callback_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  // We only support one stream at the moment.
  if (frame_types && !frame_types->empty()) {
    if ((*frame_types)[0] == kVideoFrameKey) {
      force_key_frame_ = true;
    }
  }
  RTC_DCHECK_EQ(input_image.width(), raw_->d_w);
  RTC_DCHECK_EQ(input_image.height(), raw_->d_h);

  // Set input image for use in the callback.
  // This was necessary since you need some information from input_image.
  // You can save only the necessary information (such as timestamp) instead of
  // doing this.
  input_image_ = &input_image;

  rtc::scoped_refptr<I420BufferInterface> i420_buffer =
      input_image.video_frame_buffer()->ToI420();
  // Image in vpx_image_t format.
  // Input image is const. VPX's raw image is not defined as const.
  raw_->planes[VPX_PLANE_Y] = const_cast<uint8_t*>(i420_buffer->DataY());
  raw_->planes[VPX_PLANE_U] = const_cast<uint8_t*>(i420_buffer->DataU());
  raw_->planes[VPX_PLANE_V] = const_cast<uint8_t*>(i420_buffer->DataV());
  raw_->stride[VPX_PLANE_Y] = i420_buffer->StrideY();
  raw_->stride[VPX_PLANE_U] = i420_buffer->StrideU();
  raw_->stride[VPX_PLANE_V] = i420_buffer->StrideV();

  vpx_enc_frame_flags_t flags = 0;
  if (force_key_frame_) {
    flags = VPX_EFLAG_FORCE_KF;
  }

  if (is_flexible_mode_) {
    SuperFrameRefSettings settings;

    // These structs are copied when calling vpx_codec_control,
    // therefore it is ok for them to go out of scope.
    vpx_svc_ref_frame_config enc_layer_conf;
    vpx_svc_layer_id layer_id;

    if (codec_.mode == kRealtimeVideo) {
      // Real time video not yet implemented in flexible mode.
      RTC_NOTREACHED();
    } else {
      settings = spatial_layer_->GetSuperFrameSettings(input_image.timestamp(),
                                                       force_key_frame_);
    }
    enc_layer_conf = GenerateRefsAndFlags(settings);
    layer_id.temporal_layer_id = 0;
    layer_id.spatial_layer_id = settings.start_layer;
    vpx_codec_control(encoder_, VP9E_SET_SVC_LAYER_ID, &layer_id);
    vpx_codec_control(encoder_, VP9E_SET_SVC_REF_FRAME_CONFIG, &enc_layer_conf);
  }

  RTC_CHECK_GT(codec_.maxFramerate, 0);
  uint32_t duration = 90000 / codec_.maxFramerate;
  if (vpx_codec_encode(encoder_, raw_, timestamp_, duration, flags,
                       VPX_DL_REALTIME)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  timestamp_ += duration;

  const bool end_of_picture = true;
  DeliverBufferedFrame(end_of_picture);

  return WEBRTC_VIDEO_CODEC_OK;
}

void VP9EncoderImpl::PopulateCodecSpecific(CodecSpecificInfo* codec_specific,
                                           const vpx_codec_cx_pkt& pkt,
                                           uint32_t timestamp,
                                           bool first_frame_in_picture) {
  RTC_CHECK(codec_specific != nullptr);
  codec_specific->codecType = kVideoCodecVP9;
  codec_specific->codec_name = ImplementationName();
  CodecSpecificInfoVP9* vp9_info = &(codec_specific->codecSpecific.VP9);

  vp9_info->first_frame_in_picture = first_frame_in_picture;
  // TODO(asapersson): Set correct value.
  vp9_info->inter_pic_predicted =
      (pkt.data.frame.flags & VPX_FRAME_IS_KEY) ? false : true;
  vp9_info->flexible_mode = codec_.VP9()->flexibleMode;
  vp9_info->ss_data_available =
      ((pkt.data.frame.flags & VPX_FRAME_IS_KEY) && !codec_.VP9()->flexibleMode)
          ? true
          : false;

  vpx_svc_layer_id_t layer_id = {0};
  vpx_codec_control(encoder_, VP9E_GET_SVC_LAYER_ID, &layer_id);

  RTC_CHECK_GT(num_temporal_layers_, 0);
  RTC_CHECK_GT(num_spatial_layers_, 0);
  if (num_temporal_layers_ == 1) {
    RTC_CHECK_EQ(layer_id.temporal_layer_id, 0);
    vp9_info->temporal_idx = kNoTemporalIdx;
  } else {
    vp9_info->temporal_idx = layer_id.temporal_layer_id;
  }
  if (num_spatial_layers_ == 1) {
    RTC_CHECK_EQ(layer_id.spatial_layer_id, 0);
    vp9_info->spatial_idx = kNoSpatialIdx;
  } else {
    vp9_info->spatial_idx = layer_id.spatial_layer_id;
  }
  if (layer_id.spatial_layer_id != 0) {
    vp9_info->ss_data_available = false;
  }

  // TODO(asapersson): this info has to be obtained from the encoder.
  vp9_info->temporal_up_switch = false;

  if (pkt.data.frame.flags & VPX_FRAME_IS_KEY) {
    pics_since_key_ = 0;
  } else if (first_frame_in_picture) {
    ++pics_since_key_;
  }

  const bool is_key_pic = (pics_since_key_ == 0);
  const bool is_inter_layer_pred_allowed =
      (inter_layer_pred_ == InterLayerPredMode::kOn ||
       (inter_layer_pred_ == InterLayerPredMode::kOnKeyPic && is_key_pic));

  // Always set inter_layer_predicted to true on high layer frame if inter-layer
  // prediction (ILP) is allowed even if encoder didn't actually use it.
  // Setting inter_layer_predicted to false would allow receiver to decode high
  // layer frame without decoding low layer frame. If that would happen (e.g.
  // if low layer frame is lost) then receiver won't be able to decode next high
  // layer frame which uses ILP.
  vp9_info->inter_layer_predicted =
      first_frame_in_picture ? false : is_inter_layer_pred_allowed;

  const bool is_last_layer =
      (layer_id.spatial_layer_id + 1 == num_spatial_layers_);
  vp9_info->non_ref_for_inter_layer_pred =
      is_last_layer ? true : !is_inter_layer_pred_allowed;

  // Always populate this, so that the packetizer can properly set the marker
  // bit.
  vp9_info->num_spatial_layers = num_spatial_layers_;

  vp9_info->num_ref_pics = 0;
  if (vp9_info->flexible_mode) {
    vp9_info->gof_idx = kNoGofIdx;
    vp9_info->num_ref_pics = num_ref_pics_[layer_id.spatial_layer_id];
    for (int i = 0; i < num_ref_pics_[layer_id.spatial_layer_id]; ++i) {
      vp9_info->p_diff[i] = p_diff_[layer_id.spatial_layer_id][i];
    }
  } else {
    vp9_info->gof_idx =
        static_cast<uint8_t>(pics_since_key_ % gof_.num_frames_in_gof);
    vp9_info->temporal_up_switch = gof_.temporal_up_switch[vp9_info->gof_idx];
  }

  if (vp9_info->ss_data_available) {
    vp9_info->spatial_layer_resolution_present = true;
    for (size_t i = 0; i < vp9_info->num_spatial_layers; ++i) {
      vp9_info->width[i] = codec_.width *
                           svc_params_.scaling_factor_num[i] /
                           svc_params_.scaling_factor_den[i];
      vp9_info->height[i] = codec_.height *
                            svc_params_.scaling_factor_num[i] /
                            svc_params_.scaling_factor_den[i];
    }
    if (!vp9_info->flexible_mode) {
      vp9_info->gof.CopyGofInfoVP9(gof_);
    }
  }
}

int VP9EncoderImpl::GetEncodedLayerFrame(const vpx_codec_cx_pkt* pkt) {
  RTC_DCHECK_EQ(pkt->kind, VPX_CODEC_CX_FRAME_PKT);

  if (pkt->data.frame.sz == 0) {
    // Ignore dropped frame.
    return WEBRTC_VIDEO_CODEC_OK;
  }

  vpx_svc_layer_id_t layer_id = {0};
  vpx_codec_control(encoder_, VP9E_GET_SVC_LAYER_ID, &layer_id);

  const bool first_frame_in_picture = encoded_image_._length == 0;
  // Ensure we don't buffer layers of previous picture (superframe).
  RTC_DCHECK(first_frame_in_picture || layer_id.spatial_layer_id > 0);

  const bool end_of_picture = false;
  DeliverBufferedFrame(end_of_picture);

  if (pkt->data.frame.sz > encoded_image_._size) {
    delete[] encoded_image_._buffer;
    encoded_image_._size = pkt->data.frame.sz;
    encoded_image_._buffer = new uint8_t[encoded_image_._size];
  }
  memcpy(encoded_image_._buffer, pkt->data.frame.buf, pkt->data.frame.sz);
  encoded_image_._length = pkt->data.frame.sz;

  if (is_flexible_mode_ && codec_.mode == kScreensharing) {
    spatial_layer_->LayerFrameEncoded(
        static_cast<unsigned int>(encoded_image_._length),
        layer_id.spatial_layer_id);
  }

  const bool is_key_frame =
      (pkt->data.frame.flags & VPX_FRAME_IS_KEY) ? true : false;
  // Ensure encoder issued key frame on request.
  RTC_DCHECK(is_key_frame || !force_key_frame_);

  // Check if encoded frame is a key frame.
  encoded_image_._frameType = kVideoFrameDelta;
  if (is_key_frame) {
    encoded_image_._frameType = kVideoFrameKey;
    force_key_frame_ = false;
  }
  RTC_DCHECK_LE(encoded_image_._length, encoded_image_._size);

  memset(&codec_specific_, 0, sizeof(codec_specific_));
  PopulateCodecSpecific(&codec_specific_, *pkt, input_image_->timestamp(),
                        first_frame_in_picture);

  TRACE_COUNTER1("webrtc", "EncodedFrameSize", encoded_image_._length);
  encoded_image_._timeStamp = input_image_->timestamp();
  encoded_image_.capture_time_ms_ = input_image_->render_time_ms();
  encoded_image_.rotation_ = input_image_->rotation();
  encoded_image_.content_type_ = (codec_.mode == kScreensharing)
                                     ? VideoContentType::SCREENSHARE
                                     : VideoContentType::UNSPECIFIED;
  encoded_image_._encodedHeight =
      pkt->data.frame.height[layer_id.spatial_layer_id];
  encoded_image_._encodedWidth =
      pkt->data.frame.width[layer_id.spatial_layer_id];
  encoded_image_.timing_.flags = TimingFrameFlags::kInvalid;
  int qp = -1;
  vpx_codec_control(encoder_, VP8E_GET_LAST_QUANTIZER, &qp);
  encoded_image_.qp_ = qp;

  return WEBRTC_VIDEO_CODEC_OK;
}

void VP9EncoderImpl::DeliverBufferedFrame(bool end_of_picture) {
  if (encoded_image_._length > 0) {
    codec_specific_.codecSpecific.VP9.end_of_picture = end_of_picture;

    // No data partitioning in VP9, so 1 partition only.
    int part_idx = 0;
    RTPFragmentationHeader frag_info;
    frag_info.VerifyAndAllocateFragmentationHeader(1);
    frag_info.fragmentationOffset[part_idx] = 0;
    frag_info.fragmentationLength[part_idx] = encoded_image_._length;
    frag_info.fragmentationPlType[part_idx] = 0;
    frag_info.fragmentationTimeDiff[part_idx] = 0;

    encoded_complete_callback_->OnEncodedImage(encoded_image_, &codec_specific_,
                                               &frag_info);
    encoded_image_._length = 0;
  }
}

vpx_svc_ref_frame_config VP9EncoderImpl::GenerateRefsAndFlags(
    const SuperFrameRefSettings& settings) {
  static const vpx_enc_frame_flags_t kAllFlags =
      VP8_EFLAG_NO_REF_ARF | VP8_EFLAG_NO_REF_GF | VP8_EFLAG_NO_REF_LAST |
      VP8_EFLAG_NO_UPD_LAST | VP8_EFLAG_NO_UPD_ARF | VP8_EFLAG_NO_UPD_GF;
  vpx_svc_ref_frame_config sf_conf = {};
  if (settings.is_keyframe) {
    // Used later on to make sure we don't make any invalid references.
    memset(buffer_updated_at_frame_, -1, sizeof(buffer_updated_at_frame_));
    for (int layer = settings.start_layer; layer <= settings.stop_layer;
         ++layer) {
      num_ref_pics_[layer] = 0;
      buffer_updated_at_frame_[settings.layer[layer].upd_buf] = frames_encoded_;
      // When encoding a keyframe only the alt_fb_idx is used
      // to specify which layer ends up in which buffer.
      sf_conf.alt_fb_idx[layer] = settings.layer[layer].upd_buf;
    }
  } else {
    for (int layer_idx = settings.start_layer; layer_idx <= settings.stop_layer;
         ++layer_idx) {
      vpx_enc_frame_flags_t layer_flags = kAllFlags;
      num_ref_pics_[layer_idx] = 0;
      int8_t refs[3] = {settings.layer[layer_idx].ref_buf1,
                        settings.layer[layer_idx].ref_buf2,
                        settings.layer[layer_idx].ref_buf3};

      for (unsigned int ref_idx = 0; ref_idx < kMaxVp9RefPics; ++ref_idx) {
        if (refs[ref_idx] == -1)
          continue;

        RTC_DCHECK_GE(refs[ref_idx], 0);
        RTC_DCHECK_LE(refs[ref_idx], 7);
        // Easier to remove flags from all flags rather than having to
        // build the flags from 0.
        switch (num_ref_pics_[layer_idx]) {
          case 0: {
            sf_conf.lst_fb_idx[layer_idx] = refs[ref_idx];
            layer_flags &= ~VP8_EFLAG_NO_REF_LAST;
            break;
          }
          case 1: {
            sf_conf.gld_fb_idx[layer_idx] = refs[ref_idx];
            layer_flags &= ~VP8_EFLAG_NO_REF_GF;
            break;
          }
          case 2: {
            sf_conf.alt_fb_idx[layer_idx] = refs[ref_idx];
            layer_flags &= ~VP8_EFLAG_NO_REF_ARF;
            break;
          }
        }
        // Make sure we don't reference a buffer that hasn't been
        // used at all or hasn't been used since a keyframe.
        RTC_DCHECK_NE(buffer_updated_at_frame_[refs[ref_idx]], -1);

        p_diff_[layer_idx][num_ref_pics_[layer_idx]] =
            frames_encoded_ - buffer_updated_at_frame_[refs[ref_idx]];
        num_ref_pics_[layer_idx]++;
      }

      bool upd_buf_same_as_a_ref = false;
      if (settings.layer[layer_idx].upd_buf != -1) {
        for (unsigned int ref_idx = 0; ref_idx < kMaxVp9RefPics; ++ref_idx) {
          if (settings.layer[layer_idx].upd_buf == refs[ref_idx]) {
            switch (ref_idx) {
              case 0: {
                layer_flags &= ~VP8_EFLAG_NO_UPD_LAST;
                break;
              }
              case 1: {
                layer_flags &= ~VP8_EFLAG_NO_UPD_GF;
                break;
              }
              case 2: {
                layer_flags &= ~VP8_EFLAG_NO_UPD_ARF;
                break;
              }
            }
            upd_buf_same_as_a_ref = true;
            break;
          }
        }
        if (!upd_buf_same_as_a_ref) {
          // If we have three references and a buffer is specified to be
          // updated, then that buffer must be the same as one of the
          // three references.
          RTC_CHECK_LT(num_ref_pics_[layer_idx], kMaxVp9RefPics);

          sf_conf.alt_fb_idx[layer_idx] = settings.layer[layer_idx].upd_buf;
          layer_flags ^= VP8_EFLAG_NO_UPD_ARF;
        }

        int updated_buffer = settings.layer[layer_idx].upd_buf;
        buffer_updated_at_frame_[updated_buffer] = frames_encoded_;
        sf_conf.frame_flags[layer_idx] = layer_flags;
      }
    }
  }
  ++frames_encoded_;
  return sf_conf;
}

int VP9EncoderImpl::SetChannelParameters(uint32_t packet_loss, int64_t rtt) {
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP9EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_complete_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

const char* VP9EncoderImpl::ImplementationName() const {
  return "libvpx";
}

bool VP9Decoder::IsSupported() {
  return true;
}

std::unique_ptr<VP9Decoder> VP9Decoder::Create() {
  return rtc::MakeUnique<VP9DecoderImpl>();
}

VP9DecoderImpl::VP9DecoderImpl()
    : decode_complete_callback_(nullptr),
      inited_(false),
      decoder_(nullptr),
      key_frame_required_(true) {}

VP9DecoderImpl::~VP9DecoderImpl() {
  inited_ = true;  // in order to do the actual release
  Release();
  int num_buffers_in_use = frame_buffer_pool_.GetNumBuffersInUse();
  if (num_buffers_in_use > 0) {
    // The frame buffers are reference counted and frames are exposed after
    // decoding. There may be valid usage cases where previous frames are still
    // referenced after ~VP9DecoderImpl that is not a leak.
    RTC_LOG(LS_INFO) << num_buffers_in_use << " Vp9FrameBuffers are still "
                     << "referenced during ~VP9DecoderImpl.";
  }
}

int VP9DecoderImpl::InitDecode(const VideoCodec* inst, int number_of_cores) {
  int ret_val = Release();
  if (ret_val < 0) {
    return ret_val;
  }
  if (decoder_ == nullptr) {
    decoder_ = new vpx_codec_ctx_t;
  }
  vpx_codec_dec_cfg_t cfg;
  // Setting number of threads to a constant value (1)
  cfg.threads = 1;
  cfg.h = cfg.w = 0;  // set after decode
  vpx_codec_flags_t flags = 0;
  if (vpx_codec_dec_init(decoder_, vpx_codec_vp9_dx(), &cfg, flags)) {
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }

  if (!frame_buffer_pool_.InitializeVpxUsePool(decoder_)) {
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }

  inited_ = true;
  // Always start with a complete key frame.
  key_frame_required_ = true;
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP9DecoderImpl::Decode(const EncodedImage& input_image,
                           bool missing_frames,
                           const RTPFragmentationHeader* fragmentation,
                           const CodecSpecificInfo* codec_specific_info,
                           int64_t /*render_time_ms*/) {
  if (!inited_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (decode_complete_callback_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  // Always start with a complete key frame.
  if (key_frame_required_) {
    if (input_image._frameType != kVideoFrameKey)
      return WEBRTC_VIDEO_CODEC_ERROR;
    // We have a key frame - is it complete?
    if (input_image._completeFrame) {
      key_frame_required_ = false;
    } else {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }
  vpx_codec_iter_t iter = nullptr;
  vpx_image_t* img;
  uint8_t* buffer = input_image._buffer;
  if (input_image._length == 0) {
    buffer = nullptr;  // Triggers full frame concealment.
  }
  // During decode libvpx may get and release buffers from |frame_buffer_pool_|.
  // In practice libvpx keeps a few (~3-4) buffers alive at a time.
  if (vpx_codec_decode(decoder_, buffer,
                       static_cast<unsigned int>(input_image._length), 0,
                       VPX_DL_REALTIME)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  // |img->fb_priv| contains the image data, a reference counted Vp9FrameBuffer.
  // It may be released by libvpx during future vpx_codec_decode or
  // vpx_codec_destroy calls.
  img = vpx_codec_get_frame(decoder_, &iter);
  int qp;
  vpx_codec_err_t vpx_ret =
      vpx_codec_control(decoder_, VPXD_GET_LAST_QUANTIZER, &qp);
  RTC_DCHECK_EQ(vpx_ret, VPX_CODEC_OK);
  int ret =
      ReturnFrame(img, input_image._timeStamp, input_image.ntp_time_ms_, qp);
  if (ret != 0) {
    return ret;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP9DecoderImpl::ReturnFrame(const vpx_image_t* img,
                                uint32_t timestamp,
                                int64_t ntp_time_ms,
                                int qp) {
  if (img == nullptr) {
    // Decoder OK and nullptr image => No show frame.
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  // This buffer contains all of |img|'s image data, a reference counted
  // Vp9FrameBuffer. (libvpx is done with the buffers after a few
  // vpx_codec_decode calls or vpx_codec_destroy).
  Vp9FrameBufferPool::Vp9FrameBuffer* img_buffer =
      static_cast<Vp9FrameBufferPool::Vp9FrameBuffer*>(img->fb_priv);
  // The buffer can be used directly by the VideoFrame (without copy) by
  // using a WrappedI420Buffer.
  rtc::scoped_refptr<WrappedI420Buffer> img_wrapped_buffer(
      new rtc::RefCountedObject<webrtc::WrappedI420Buffer>(
          img->d_w, img->d_h, img->planes[VPX_PLANE_Y],
          img->stride[VPX_PLANE_Y], img->planes[VPX_PLANE_U],
          img->stride[VPX_PLANE_U], img->planes[VPX_PLANE_V],
          img->stride[VPX_PLANE_V],
          // WrappedI420Buffer's mechanism for allowing the release of its frame
          // buffer is through a callback function. This is where we should
          // release |img_buffer|.
          rtc::KeepRefUntilDone(img_buffer)));

  VideoFrame decoded_image(img_wrapped_buffer, timestamp,
                           0 /* render_time_ms */, webrtc::kVideoRotation_0);
  decoded_image.set_ntp_time_ms(ntp_time_ms);

  decode_complete_callback_->Decoded(decoded_image, rtc::nullopt, qp);
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP9DecoderImpl::RegisterDecodeCompleteCallback(
    DecodedImageCallback* callback) {
  decode_complete_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP9DecoderImpl::Release() {
  int ret_val = WEBRTC_VIDEO_CODEC_OK;

  if (decoder_ != nullptr) {
    if (inited_) {
      // When a codec is destroyed libvpx will release any buffers of
      // |frame_buffer_pool_| it is currently using.
      if (vpx_codec_destroy(decoder_)) {
        ret_val = WEBRTC_VIDEO_CODEC_MEMORY;
      }
    }
    delete decoder_;
    decoder_ = nullptr;
  }
  // Releases buffers from the pool. Any buffers not in use are deleted. Buffers
  // still referenced externally are deleted once fully released, not returning
  // to the pool.
  frame_buffer_pool_.ClearPool();
  inited_ = false;
  return ret_val;
}

const char* VP9DecoderImpl::ImplementationName() const {
  return "libvpx";
}

}  // namespace webrtc
