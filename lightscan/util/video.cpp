/* Copyright 2016 Carnegie Mellon University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lightscan/storage/storage_backend.h"
#include "lightscan/util/video.h"

#include <cassert>

#include <cuda.h>
#include <nvcuvid.h>

// For video
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavformat/movenc.h"
#include "libavutil/error.h"
#include "libswscale/swscale.h"

#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"

// For hardware decode
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda.h"
}

// Stolen from libavformat/movenc.h
#define FF_MOV_FLAG_FASTSTART             (1 <<  7)

namespace lightscan {

pthread_mutex_t av_mutex;

namespace {

struct BufferData {
  uint8_t *ptr;
  size_t size; // size left in the buffer

  uint8_t *orig_ptr;
  size_t initial_size;
};

// For custom AVIOContext that loads from memory

int read_packet(void *opaque, uint8_t *buf, int buf_size) {
  BufferData* bd = (BufferData*)opaque;
  buf_size = std::min(static_cast<size_t>(buf_size), bd->size);
  /* copy internal buffer data to buf */
  memcpy(buf, bd->ptr, buf_size);
  bd->ptr  += buf_size;
  bd->size -= buf_size;
  return buf_size;
}

int64_t seek(void *opaque, int64_t offset, int whence) {
  BufferData* bd = (BufferData*)opaque;
  {
    switch (whence)
    {
    case SEEK_SET:
      bd->ptr = bd->orig_ptr + offset;
      bd->size = bd->initial_size - offset;
      break;
    case SEEK_CUR:
      bd->ptr += offset;
      bd->size -= offset;
      break;
    case SEEK_END:
      bd->ptr = bd->orig_ptr + bd->initial_size;
      bd->size = 0;
      break;
    case AVSEEK_SIZE:
      return bd->initial_size;
      break;
    }
    return bd->initial_size - bd->size;
  }
}

// Taken directly from ffmpeg_cuvid.c
typedef struct CUVIDContext {
    AVBufferRef *hw_frames_ctx;
} CUVIDContext;

typedef struct CodecHardwareInfo {
    /* hwaccel options */
    char  *hwaccel_device;
    enum AVPixelFormat hwaccel_output_format;

    /* hwaccel context */
    void  *hwaccel_ctx;
    void (*hwaccel_uninit)(AVCodecContext *s);
    int  (*hwaccel_get_buffer)(AVCodecContext *s, AVFrame *frame, int flags);
    int  (*hwaccel_retrieve_data)(AVCodecContext *s, AVFrame *frame);
    enum AVPixelFormat hwaccel_pix_fmt;
    enum AVPixelFormat hwaccel_retrieved_pix_fmt;
    AVBufferRef *hw_frames_ctx;
} CodecHardwareInfo;

void cuvid_uninit(AVCodecContext *s) {
  CodecHardwareInfo *ist = (CodecHardwareInfo*)s->opaque;
  CUVIDContext *ctx = (CUVIDContext*)ist->hwaccel_ctx;

  ist->hwaccel_uninit        = NULL;
  ist->hwaccel_get_buffer    = NULL;
  ist->hwaccel_retrieve_data = NULL;

  //av_buffer_unref(&ctx->hw_frames_ctx);
  av_buffer_unref(&ist->hw_frames_ctx);

  av_freep(&ist->hwaccel_ctx);
  av_freep(&s->hwaccel_context);

  av_freep(&s->opaque);
}

void cuvid_ctx_free(AVHWDeviceContext *ctx) {
  AVCUDADeviceContext *hwctx = (AVCUDADeviceContext*)ctx->hwctx;
  cuCtxDestroy(hwctx->cuda_ctx);
}

int cuvid_init(AVCodecContext *cc, CUcontext cuda_ctx) {
  CodecHardwareInfo *ist;
  CUVIDContext *ctx = NULL;
  AVBufferRef *hw_device_ctx = NULL;
  AVCUDADeviceContext *device_hwctx;
  AVHWDeviceContext *device_ctx;
  AVHWFramesContext *hwframe_ctx;
  CUdevice device;
  CUcontext dummy;
  CUresult err;
  int ret = 0;

  ist = (CodecHardwareInfo*)cc->opaque;

  if (!ist) {
    ist = (CodecHardwareInfo*)av_mallocz(sizeof(*ist));
    if (!ist) {
      ret = AVERROR(ENOMEM);
      goto error;
    }
    cc->opaque = ist;
  }

  av_log(NULL, AV_LOG_VERBOSE, "Setting up CUVID decoder\n");

  if (ist->hwaccel_ctx) {
    ctx = (CUVIDContext*)ist->hwaccel_ctx;
  } else {
    ctx = (CUVIDContext*)av_mallocz(sizeof(*ctx));
    if (!ctx) {
      ret = AVERROR(ENOMEM);
      goto error;
    }
  }

  if (!hw_device_ctx) {
    hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if (!hw_device_ctx) {
      av_log(NULL, AV_LOG_ERROR, "av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA) failed\n");
      ret = AVERROR(ENOMEM);
      goto error;
    }

    err = cuInit(0);
    if (err != CUDA_SUCCESS) {
      av_log(NULL, AV_LOG_ERROR, "Could not initialize the CUDA driver API\n");
      ret = AVERROR_UNKNOWN;
      goto error;
    }

    // err = cuDeviceGet(&device, gpu_device_id); 
    // if (err != CUDA_SUCCESS) {
    //   av_log(NULL, AV_LOG_ERROR, "Could not get the device number %d\n", 0);
    //   ret = AVERROR_UNKNOWN;
    //   goto error;
    // }

    // err = cuCtxCreate(&cuda_ctx, CU_CTX_SCHED_BLOCKING_SYNC, device);
    // if (err != CUDA_SUCCESS) {
    //   av_log(NULL, AV_LOG_ERROR, "Error creating a CUDA context\n");
    //   ret = AVERROR_UNKNOWN;
    //   goto error;
    // }

    device_ctx = (AVHWDeviceContext*)hw_device_ctx->data;
    device_ctx->free = cuvid_ctx_free;

    device_hwctx = (AVCUDADeviceContext*)device_ctx->hwctx;
    device_hwctx->cuda_ctx = cuda_ctx;

    // err = cuCtxPopCurrent(&dummy);
    // if (err != CUDA_SUCCESS) {
    //   av_log(NULL, AV_LOG_ERROR, "cuCtxPopCurrent failed\n");
    //   ret = AVERROR_UNKNOWN;
    //   goto error;
    // }

    ret = av_hwdevice_ctx_init(hw_device_ctx);
    if (ret < 0) {
      av_log(NULL, AV_LOG_ERROR, "av_hwdevice_ctx_init failed\n");
      goto error;
    }
  } else {
    device_ctx = (AVHWDeviceContext*)hw_device_ctx->data;
    device_hwctx = (AVCUDADeviceContext*)device_ctx->hwctx;
    cuda_ctx = device_hwctx->cuda_ctx;
  }

  if (device_ctx->type != AV_HWDEVICE_TYPE_CUDA) {
    av_log(NULL, AV_LOG_ERROR, "Hardware device context is already initialized for a diffrent hwaccel.\n");
    ret = AVERROR(EINVAL);
    goto error;
  }

  if (!ctx->hw_frames_ctx) {
    ctx->hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!ctx->hw_frames_ctx) {
      av_log(NULL, AV_LOG_ERROR, "av_hwframe_ctx_alloc failed\n");
      ret = AVERROR(ENOMEM);
      goto error;
    }
    cc->hw_frames_ctx = ctx->hw_frames_ctx;
  }

  /* This is a bit hacky, av_hwframe_ctx_init is called by the cuvid decoder
   * once it has probed the neccesary format information. But as filters/nvenc
   * need to know the format/sw_format, set them here so they are happy.
   * This is fine as long as CUVID doesn't add another supported pix_fmt.
   */
  hwframe_ctx = (AVHWFramesContext*)ctx->hw_frames_ctx->data;
  hwframe_ctx->format = AV_PIX_FMT_CUDA;
  hwframe_ctx->sw_format = AV_PIX_FMT_NV12;
  //hwframe_ctx->width     = cc_->coded_width;
  //hwframe_ctx->height    = cc_->coded_height;

  if (!ist->hwaccel_ctx) {
    ist->hwaccel_ctx = ctx;
    ist->hw_frames_ctx = av_buffer_ref(ctx->hw_frames_ctx);

    ist->hwaccel_uninit = cuvid_uninit;

    if (!ist->hw_frames_ctx) {
      av_log(NULL, AV_LOG_ERROR, "av_buffer_ref failed\n");
      ret = AVERROR(ENOMEM);
      goto error;
    }
  }

  return 0;

error:
  av_freep(&ctx);
  return ret;

cancel:
  av_log(NULL, AV_LOG_ERROR,
         "CUVID hwaccel requested, but impossible to achive.\n");
  return AVERROR(EINVAL);
}


}

// For custom AVIOContext that loads from memory

struct CodecState {
  AVPacket av_packet;
  AVFrame* picture;
  // Input objects
  AVFormatContext* format_context;
  AVIOContext* io_context;
  AVCodec* in_codec;
  AVCodecContext* in_cc;
  int video_stream_index;
  // Output objets
  AVStream* out_stream;
  AVFormatContext* out_format_context;
  AVCodecContext* out_cc;
};

void set_video_stream_settings(AVStream* stream,
                               AVCodecContext* c,
                               AVCodec* codec,
                               int width, int height,
                               AVRational avg_frame_rate,
                               AVRational time_base,
                               int bit_rate,
                               int bit_rate_tolerance,
                               int gop_size,
                               AVRational sample_aspect_ratio)
{
  stream->r_frame_rate.num = avg_frame_rate.num;
  stream->r_frame_rate.den = avg_frame_rate.den;
  stream->time_base = avg_frame_rate;

  c->codec_id = codec->id;
  c->codec_type = AVMEDIA_TYPE_VIDEO;
  c->profile = FF_PROFILE_H264_HIGH;
  c->pix_fmt = AV_PIX_FMT_YUV420P;
  c->width = width;
  c->height = height;
  c->time_base.num = time_base.num;
  c->time_base.den = time_base.den;
  av_opt_set_int(c, "crf", 15, AV_OPT_SEARCH_CHILDREN);
  // c->bit_rate_tolerance = bit_rate_tolerance;
  // c->gop_size = gop_size;

  //c->sample_aspect_ratio = sample_aspect_ratio;
}

CodecState setup_video_codec(BufferData* buffer) {
  printf("Setting up video codec\n");
  CodecState state;
  av_init_packet(&state.av_packet);
  state.picture = av_frame_alloc();
  state.format_context = avformat_alloc_context();

  size_t avio_context_buffer_size = 4096;
  uint8_t* avio_context_buffer =
    static_cast<uint8_t*>(av_malloc(avio_context_buffer_size));
  state.io_context =
    avio_alloc_context(avio_context_buffer, avio_context_buffer_size,
                       0, buffer, &read_packet, NULL, &seek);
  state.format_context->pb = state.io_context;

  // Read file header
  printf("Opening input file to read format\n");
  if (avformat_open_input(&state.format_context, NULL, NULL, NULL) < 0) {
    fprintf(stderr, "open input failed\n");
    assert(false);
  }
  // Some formats don't have a header
  if (avformat_find_stream_info(state.format_context, NULL) < 0) {
    fprintf(stderr, "find stream info failed\n");
    assert(false);
  }

  av_dump_format(state.format_context, 0, NULL, 0);

  // Find the best video stream in our input video
  state.video_stream_index =
    av_find_best_stream(state.format_context,
                        AVMEDIA_TYPE_VIDEO,
                        -1 /* auto select */,
                        -1 /* no related stream */,
                        &state.in_codec,
                        0 /* flags */);
  if (state.video_stream_index < 0) {
    fprintf(stderr, "could not find best stream\n");
    assert(false);
  }

  AVStream const* const in_stream =
    state.format_context->streams[state.video_stream_index];

  state.in_cc = in_stream->codec;

  state.in_cc = avcodec_find_decoder_by_name("h264_cuvid");
  if (state.in_cc == NULL) {
    fprintf(stderr, "could not find hardware decoder\n");
    exit(EXIT_FAILURE);
  }

  CUcontext cuda_context;
  CUD_CHECK(cuDevicePrimaryCtxRetain(&cuda_context, 0));

  if (cuvid_init(state.in_cc, cuda_context) < 0) {
    fprintf(stderr, "could not init cuvid codec context\n");
    exit(EXIT_FAILURE);
  }

  if (avcodec_open2(state.in_cc, state.in_codec, NULL) < 0) {
    fprintf(stderr, "could not open codec\n");
    assert(false);
  }

  // Setup output codec and stream that we will use to reencode the input video

#ifdef HAVE_X264_ENCODER
  AVOutputFormat* output_format = av_guess_format("mp4", NULL, NULL);
  if (output_format == NULL) {
    fprintf(stderr, "output format could not be guessed\n");
    assert(false);
  }
  avformat_alloc_output_context2(&state.out_format_context,
                                 output_format,
                                 NULL,
                                 NULL);
  printf("output format\n");
  fflush(stdout);

  AVCodec* out_codec = avcodec_find_encoder_by_name("libx264");
  if (out_codec == NULL) {
    fprintf(stderr, "could not find encoder for codec name\n");
    assert(false);
  }

  state.out_stream =
    avformat_new_stream(state.out_format_context, out_codec);
  if (state.out_stream == NULL) {
    fprintf(stderr, "Could not allocate stream\n");
    exit(1);
  }

  state.out_stream->id = state.out_format_context->nb_streams - 1;
  AVCodecContext* out_cc = state.out_stream->codec;
  state.out_cc = out_cc;
  avcodec_get_context_defaults3(out_cc, out_codec);

  set_video_stream_settings(
    state.out_stream,
    out_cc,
    out_codec,
    state.in_cc->coded_width, state.in_cc->coded_height,
    in_stream->avg_frame_rate,
    in_stream->time_base,
    state.in_cc->bit_rate,
    state.in_cc->bit_rate_tolerance,
    state.in_cc->gop_size,
    in_stream->sample_aspect_ratio);

  if (output_format->flags & AVFMT_GLOBALHEADER)
    state.out_cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  state.out_stream->sample_aspect_ratio = in_stream->sample_aspect_ratio;

  if (avcodec_open2(out_cc, out_codec, NULL) < 0) {
    fprintf(stderr, "Could not open video codec\n");
    exit(1);
  }

  // if (codec->capabilities & CODEC_CAP_TRUNCATED) {
  //   codec_context->flags |= CODEC_FLAG_TRUNCATED;
  // }

/* For some codecs, such as msmpeg4 and mpeg4, width and height
   MUST be initialized there because this information is not
   available in the bitstream. */

/* the codec gives us the frame size, in samples */
  // codec_context->width = width;
  // codec_context->height = height;
  // codec_context->pix_fmt = AV_AV_PIX_FMT_YUV420P;
  // codec_context->bit_rate = format_;

  // Set fast start to move mov to the end
  MOVMuxContext *mov = NULL;

  mov = (MOVMuxContext *)state.out_format_context->priv_data;
  mov->flags |= FF_MOV_FLAG_FASTSTART;

#endif

  return state;
}

void cleanup_video_codec(CodecState state) {
  avformat_free_context(state.out_format_context);
}

void write_video_metadata(
  WriteFile* file,
  const VideoMetadata& metadata)
{
  // Frames
  StoreResult result;
  EXP_BACKOFF(
    file->append(sizeof(int32_t),
                 reinterpret_cast<const char*>(&metadata.frames)),
    result);
  assert(result == StoreResult::Success);

  // Width
  EXP_BACKOFF(
    file->append(sizeof(int32_t),
                 reinterpret_cast<const char*>(&metadata.width)),
    result);
  assert(result == StoreResult::Success);

  // Height
  EXP_BACKOFF(
    file->append(sizeof(int32_t),
                 reinterpret_cast<const char*>(&metadata.height)),
    result);
  assert(result == StoreResult::Success);

  // Codec type
  EXP_BACKOFF(
    file->append(sizeof(cudaVideoCodec),
                 reinterpret_cast<const char*>(&metadata.codec_type)),
    result);
  assert(result == StoreResult::Success);

  // Chroma format
  EXP_BACKOFF(
    file->append(sizeof(cudaVideoChromaFormat),
                 reinterpret_cast<const char*>(&metadata.chroma_format)),
    result);
  assert(result == StoreResult::Success);
}

void write_keyframe_info(
  WriteFile* file,
  const std::vector<int>& keyframe_positions,
  const std::vector<int64_t>& keyframe_timestamps)
{
  assert(keyframe_positions.size() == keyframe_timestamps.size());

  size_t num_keyframes = keyframe_positions.size();

  StoreResult result;
  EXP_BACKOFF(
    file->append(sizeof(size_t), reinterpret_cast<char*>(&num_keyframes)),
    result);
  assert(result == StoreResult::Success);

  EXP_BACKOFF(
    file->append(sizeof(int) * num_keyframes,
                 reinterpret_cast<const char*>(keyframe_positions.data())),
    result);
  EXP_BACKOFF(
    file->append(sizeof(int64_t) * num_keyframes,
                 reinterpret_cast<const char*>(keyframe_timestamps.data())),
    result);
}

}


//   // avcodec_close(codec_context);
//   // av_free(codec_context);

//   // Cleanup
//   pthread_mutex_lock(&av_mutex);
//   avformat_close_input(&format_context);
//   pthread_mutex_unlock(&av_mutex);
//   av_freep(&io_context->buffer);
//   av_freep(&io_context);

}

VideoDecoder::VideoDecoder(
  CUcontext cuda_context,
  VideoMetadata metadata)
  : cuda_context_(cuda_context),
    metadata_(metadata),
    parser_(nullptr),
    decoder_(nullptr),
    next_frame_(0),
    near_eof_(false),
    decode_time_(0)
{
  CUcontext dummy;

  CUD_CHECK(cuCtxPushCurrent(cuda_context_));

  CUVIDPARSERPARAMS cuparseinfo = {0};

  cuparseinfo.CodecType = metadata.codec_type;
  cuparseinfo.ulMaxNumDecodeSurfaces = MAX_FRAME_COUNT;
  cuparseinfo.ulMaxDisplayDelay = 4;
  cuparseinfo.pUserData = this;
  cuparseinfo.pfnSequenceCallback = VideoDecoder::cuvid_handle_video_sequence;
  cuparseinfo.pfnDecodePicture = VideoDecoder::cuvid_handle_picture_decode;
  cuparseinfo.pfnDisplayPicture = VideoDecoder::cuvid_handle_picture_display;

  CU_CHECK(cuvidCreateVideoParser(&parser_, &cuparseinfo));

  CUVIDDECODECREATEINFO cuinfo = {0};
  // HACK(apoms): Hardcode for kcam videos for now
  cuinfo.CodecType = metadata.codec_type;
  cuinfo.ChromaFormat = metadata.chroma_format;
  cuinfo.OutputFormat = cudaVideoSurfaceFormat_NV12;

  cuinfo.ulWidth = metadata.width;
  cuinfo.ulHeight = metadata.height;
  cuinfo.ulTargetWidth = cuinfo.ulWidth;
  cuinfo.ulTargetHeight = cuinfo.ulHeight;

  cuinfo.target_rect.left = 0;
  cuinfo.target_rect.top = 0;
  cuinfo.target_rect.right = cuinfo.ulWidth;
  cuinfo.target_rect.bottom = cuinfo.ulHeight;

  cuinfo.ulNumDecodeSurfaces = 20;
  cuinfo.ulNumOutputSurfaces = 1;
  cuinfo.ulCreationFlags = cudaVideoCreate_PreferCUVID;

  cuinfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;

  CU_CHECK(cuvidCreateDecoder(&decoder_, &cuinfo));

  CU_CHECK(cuCtxPopCurrent(&dummy));
}

VideoDecoder::~VideoDecoder() {
  if (parser_) {
    cuvidDestroyVideoParser(parser_);
  }

  if (decoder_) {
    cuvidDestroyDecoder(decoder_);
  }
}

bool VideoDecoder::decode(
  char* encoded_buffer,
  size_t encoded_size,
  char* decoded_buffer,
  size_t decoded_size)
{
  CUD_CHECK(cuCtxPushCurrent(cuda_context_));

  if (next_buffered_frame_ < buffered_frame_pos_) {
    int i = next_buffered_frame_++;
    if (next_buffered_frame_ >= buffered_frame_pos_) {
      next_buffered_frame_ = 1;
      buffered_frame_pos_ = 0;
    }
    next_frame_++;
    return buffered_frames_[i];
  }

  CUVIDSOURCEDATAPACKET cupkt = {0};
  cupkt.payload_size = encoded_size;
  cupkt.payload = encoded_buffer;

  CU_CHECK(cuvidParseVideoData(parser_, &cupkt));

  CUcontext dummy;
  CUD_CHECK(cuCtxPopCurrent(&dummy));

  return false;
}

double VideoDecoder::time_spent_on_decode() {
  return decode_time_;
}

void VideoDecoder::reset_timing() {
  decode_time_ = 0;
}

int VideoDecoder::cuvid_handle_video_sequence(
  void *opaque,
  CUVIDEOFORMAT* format)
{
  VideoDecoder& video_decoder = *reinterpret_cast<VideoDecoder*>(opaque);
  printf("handle video squene\n");
}

int VideoDecoder::cuvid_handle_picture_decode(
  void *opaque,
  CUVIDPICPARAMS* picparams)
{
  VideoDecoder& video_decoder = *reinterpret_cast<VideoDecoder*>(opaque);
  printf("handle picture decode\n");
}

int cuvid_handle_picture_display(
  void *opaque,
  CUVIDPARSERDISPINFO* dispinfo)
{
  VideoDecoder& video_decoder = *reinterpret_cast<VideoDecoder*>(opaque);
  printf("handle picture display\n");
}


void preprocess_video(
  StorageBackend* storage,
  const std::string& video_path,
  const std::string& processed_video_path,
  const std::string& video_metadata_path,
  const std::string& iframe_path)
{
  // The input video we will be preprocessing
  std::unique_ptr<RandomReadFile> input_file{};
  exit_on_error(
    make_unique_random_read_file(storage, video_path, input_file));

  // Load the entire input
  std::vector<char> video_bytes;
  {
    const size_t READ_SIZE = 1024 * 1024;
    size_t pos = 0;
    while (true) {
      video_bytes.resize(video_bytes.size() + READ_SIZE);
      size_t size_read;
      StoreResult result;
      EXP_BACKOFF(
        input_file->read(pos, READ_SIZE, video_bytes.data() + pos, size_read),
        result);
      assert(result == StoreResult::Success ||
             result == StoreResult::EndOfFile);
      pos += size_read;
      if (result == StoreResult::EndOfFile) {
        video_bytes.resize(video_bytes.size() - (READ_SIZE - size_read));
        break;
      }
    }
  }

  // Setup custom buffer for libavcodec so that we can read from memory instead
  // of from a file
  BufferData buffer;
  buffer.ptr = reinterpret_cast<uint8_t*>(video_bytes.data());
  buffer.size = video_bytes.size();
  buffer.orig_ptr = buffer.ptr;
  buffer.initial_size = buffer.size;

  CodecState state = setup_video_codec(&buffer);

  VideoMetadata video_metadata;
  video_metadata.width = state.in_cc->coded_width;
  video_metadata.height = state.in_cc->coded_height;

  std::vector<char> demuxed_video_stream;
  std::vector<int> iframe_positions;
  std::vector<int64_t> iframe_byte_offsets;
  int frame = 0;
  while (true) {
    // Read from format context
    int err = av_read_frame(state.format_context, &state.av_packet);
    if (err == AVERROR_EOF) {
      av_packet_unref(&state.av_packet);
      break;
    } else if (err != 0) {
      printf("err %d\n", err);
      assert(err == 0);
    }

    if (state.av_packet.stream_index != state.video_stream_index) {
      av_packet_unref(&state.av_packet);
      continue;
    }

    /* NOTE1: some codecs are stream based (mpegvideo, mpegaudio)
       and this is the only method to use them because you cannot
       know the compressed data size before analysing it.

       BUT some other codecs (msmpeg4, mpeg4) are inherently frame
       based, so you must call them with all the data for one
       frame exactly. You must also initialize 'width' and
       'height' before initializing them. */

    /* NOTE2: some codecs allow the raw parameters (frame size,
       sample rate) to be changed at any frame. We handle this, so
       you should also take care of it */

    /* here, we use a stream based decoder (mpeg1video), so we
       feed decoder and see if it could decode a frame */
    uint8_t* orig_data = state.av_packet.data;
    int orig_size = state.av_packet.size;
    while (state.av_packet.size > 0) {
      int got_picture = 0;
      int len = avcodec_decode_video2(state.in_cc,
                                      state.picture,
                                      &got_picture,
                                      &state.av_packet);
      if (len < 0) {
        char err_msg[256];
        av_strerror(len, err_msg, 256);
        fprintf(stderr, "Error while decoding frame %d (%d): %s\n",
                frame, len, err_msg);
        assert(false);
      }
      if (got_picture) {
        state.picture->pts = frame;

        if (state.picture->key_frame == 1) {
          printf("keyframe dts %d\n",
                 state.picture->pkt_dts);
          iframe_positions.push_back(frame);
          iframe_byte_offsets.push_back(demuxed_video_stream.size());
        }
        // the picture is allocated by the decoder. no need to free
        frame++;
      }
      state.av_packet.size -= len;
      state.av_packet.data += len;
    }
    state.av_packet.data = orig_data;
    state.av_packet.size = orig_size;

    // Copy the packet size and data into our demuxed video data output so we
    // can decode it directly later without demuxing
    size_t prev_size = demuxed_video_stream.size();
    demuxed_video_stream.resize(
      prev_size + sizeof(int) + state.av_packet.size);
    memcpy(demuxed_video_stream.data() + prev_size,
           &state.av_packet.size,
           sizeof(int));
    memcpy(demuxed_video_stream.data() + prev_size + sizeof(int),
           state.av_packet.data,
           state.av_packet.size);

    av_packet_unref(&state.av_packet);
  }

// /* some codecs, such as MPEG, transmit the I and P frame with a
//    latency of one frame. You must do the following to have a
//    chance to get the last frame of the video */
  state.av_packet.data = NULL;
  state.av_packet.size = 0;

  int got_picture;
  do {
    got_picture = 0;
    int len = avcodec_decode_video2(state.in_cc,
                                    state.picture,
                                    &got_picture,
                                    &state.av_packet);
    (void)len;
    if (got_picture) {
      if (state.picture->key_frame == 1) {
        iframe_positions.push_back(frame);
        iframe_byte_offsets.push_back(demuxed_video_stream.size());
      }
      // the picture is allocated by the decoder. no need to free
      frame++;
    }
  } while (got_picture);

  CuvidContext *cuvid_ctx =
    reinterpret_cast<CuvidContext*>(state.in_cc->priv_data);

  video_metadata.frames = frame;
  video_metadata.codec_type = cuvid_ctx->codec_type;
  video_metadata.chroma_format = cuvid_ctx->chroma_format;

  // Write out our demuxed video stream
  {
    std::unique_ptr<WriteFile> output_file{};
    exit_on_error(
      make_unique_write_file(storage, processed_video_path, output_file));

    // We will process the input video and write it to this output file
    const size_t WRITE_SIZE = 16 * 1024;
    char buffer[WRITE_SIZE];
    size_t pos = 0;
    while (pos != demuxed_video_stream.size()) {
      const size_t size_to_write =
        std::min(WRITE_SIZE, demuxed_video_stream.size() - pos);
      StoreResult result;
      EXP_BACKOFF(
        output_file->append(size_to_write, demuxed_video_stream.data() + pos),
        result);
      assert(result == StoreResult::Success ||
             result == StoreResult::EndOfFile);
      pos += size_to_write;
    }
    output_file->save();
  }

  {
    std::unique_ptr<WriteFile> iframe_file;
    exit_on_error(
      make_unique_write_file(storage, iframe_path, iframe_file));

    write_keyframe_info(iframe_file.get(), iframe_positions,
                        iframe_byte_offsets);

    std::unique_ptr<WriteFile> metadata_file;
    exit_on_error(
      make_unique_write_file(storage, video_metadata_path, metadata_file));

    write_video_metadata(metadata_file.get(), video_metadata);
  }

  CUD_CHECK(cuDevicePrimaryCtxRelease(0));
}

uint64_t read_video_metadata(
  RandomReadFile* file,
  uint64_t pos,
  VideoMetadata& meta)
{
  StoreResult result;
  size_t size_read;

  // Frames
  EXP_BACKOFF(
    file->read(pos,
               sizeof(int32_t),
               reinterpret_cast<char*>(&meta.frames),
               size_read),
    result);
  assert(result == StoreResult::Success);
  assert(size_read == sizeof(int32_t));
  pos += size_read;

  // Width
  EXP_BACKOFF(
    file->read(pos,
               sizeof(int32_t),
               reinterpret_cast<char*>(&meta.width),
               size_read),
    result);
  assert(result == StoreResult::Success);
  assert(size_read == sizeof(int32_t));
  pos += size_read;

  // Height
  EXP_BACKOFF(
    file->read(pos,
               sizeof(int32_t),
               reinterpret_cast<char*>(&meta.height),
               size_read),
    result);
  assert(result == StoreResult::Success);
  assert(size_read == sizeof(int32_t));
  pos += size_read;

  // Codec type
  EXP_BACKOFF(
    file->read(pos,
               sizeof(cudaVideoCodec),
               reinterpret_cast<char*>(&meta.codec_type),
               size_read),
    result);
  assert(result == StoreResult::Success);
  assert(size_read == sizeof(cudaVideoCodec));
  pos += size_read;

  // Chroma format
  EXP_BACKOFF(
    file->read(pos,
               sizeof(cudaVideoChromaFormat),
               reinterpret_cast<char*>(&meta.chroma_format),
               size_read),
    result);
  assert(result == StoreResult::Success);
  assert(size_read == sizeof(cudaVideoChromaFormat));
  pos += size_read;

  return pos;
}

uint64_t read_keyframe_info(
  RandomReadFile* file,
  uint64_t pos,
  std::vector<int>& keyframe_positions,
  std::vector<int64_t>& keyframe_timestamps)
{
  StoreResult result;
  size_t size_read;

  size_t num_keyframes;
  // HACK(apoms): Reading just a single size_t is inefficient because
  //              the file interface does not buffer or preemptively fetch
  //              a larger block of data to amortize network overheads. We
  //              should instead read the entire file into a buffer because we
  //              know it is fairly small and then deserialize from there.
  EXP_BACKOFF(
    file->read(pos,
               sizeof(size_t),
               reinterpret_cast<char*>(&num_keyframes),
               size_read),
    result);
  assert(result == StoreResult::Success);
  assert(size_read == sizeof(size_t));
  pos += size_read;

  keyframe_positions.resize(num_keyframes);
  keyframe_timestamps.resize(num_keyframes);

  EXP_BACKOFF(
    file->read(pos,
               sizeof(int) * num_keyframes,
               reinterpret_cast<char*>(keyframe_positions.data()),
               size_read),
    result);
  assert(result == StoreResult::Success);
  assert(size_read == sizeof(int) * num_keyframes);
  pos += size_read;

  EXP_BACKOFF(
    file->read(pos,
               sizeof(int64_t) * num_keyframes,
               reinterpret_cast<char*>(keyframe_timestamps.data()),
               size_read),
    result);
  assert(result == StoreResult::Success);
  assert(size_read == sizeof(int64_t) * num_keyframes);
  pos += size_read;

  return pos;
}

}
