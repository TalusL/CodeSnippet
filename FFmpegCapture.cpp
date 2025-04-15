#include <windows.h>
#include <chrono>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

const int FPS = 30;
const int FRAME_DELAY = 1000 / FPS;
const int REGION_BORDER = 8;  // 区域边框大小

class ScreenRecorder {
public:
    ScreenRecorder() : sws_ctx(nullptr), codec_ctx(nullptr), fmt_ctx(nullptr) {}

    bool Initialize(const char* filename) {
        // 获取屏幕尺寸
        width = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);

        // 初始化FFmpeg
        avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr, filename);
        if (!fmt_ctx) return false;

        // 查找编码器
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) return false;

        // 创建视频流
        stream = avformat_new_stream(fmt_ctx, codec);
        if (!stream) return false;

        // 配置编码参数
        codec_ctx = avcodec_alloc_context3(codec);
        codec_ctx->width = width;
        codec_ctx->height = height;
        codec_ctx->time_base = {1, FPS};
        codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        codec_ctx->bit_rate = 4000000;
        avcodec_open2(codec_ctx, codec, nullptr);

        // 初始化转换上下文
        sws_ctx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                width, height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);

        // 准备帧缓冲区
        frame = av_frame_alloc();
        frame->format = codec_ctx->pix_fmt;
        frame->width = codec_ctx->width;
        frame->height = codec_ctx->height;
        av_frame_get_buffer(frame, 0);

        // 打开输出文件
        if (avio_open(&fmt_ctx->pb, filename, AVIO_FLAG_WRITE) < 0) return false;
        avformat_write_header(fmt_ctx, nullptr);

        return true;
    }

    void Record() {
        auto next_frame = std::chrono::steady_clock::now();
        while (!GetAsyncKeyState(VK_ESCAPE)) {
            CaptureFrame();
            EncodeFrame();

            next_frame += std::chrono::milliseconds(FRAME_DELAY);
            std::this_thread::sleep_until(next_frame);
        }

        // 写入尾帧
        AVPacket* pkt = av_packet_alloc();
        avcodec_send_frame(codec_ctx, nullptr);
        while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);

        av_write_trailer(fmt_ctx);
    }

    ~ScreenRecorder() {
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (fmt_ctx) avformat_free_context(fmt_ctx);
        if (sws_ctx) sws_freeContext(sws_ctx);
        if (frame) av_frame_free(&frame);
    }

private:
    void CaptureFrame() {
        HDC hdcScreen = GetDC(nullptr);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
        SelectObject(hdcMem, hBitmap);
        BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);

        // 绘制区域边框
        HBRUSH borderBrush = CreateSolidBrush(RGB(255, 0, 0));
        RECT borderRect = {0, 0, width, height};
        FrameRect(hdcMem, &borderRect, borderBrush);
        DeleteObject(borderBrush);

        // 绘制鼠标
        DrawCursor(hdcMem);

        // 获取位图数据
        BITMAPINFOHEADER bi = {0};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = width;
        bi.biHeight = -height;  // 顶部优先
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        if (captureBuffer.size() < width * height * 4) {
            captureBuffer.resize(width * height * 4);
        }

        GetDIBits(hdcMem, hBitmap, 0, height, captureBuffer.data(),
                 (BITMAPINFO*)&bi, DIB_RGB_COLORS);

        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
    }

    void DrawCursor(HDC hdc) {
        CURSORINFO ci = { sizeof(ci) };
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
            ICONINFO iconInfo;
            if (GetIconInfo(ci.hCursor, &iconInfo)) {
                int x = ci.ptScreenPos.x - iconInfo.xHotspot;
                int y = ci.ptScreenPos.y - iconInfo.yHotspot;
                DrawIconEx(hdc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                DeleteObject(iconInfo.hbmColor);
                DeleteObject(iconInfo.hbmMask);
            }
        }
    }

    void EncodeFrame() {
        // 转换颜色空间
        const uint8_t* srcData[4] = { captureBuffer.data(), nullptr, nullptr, nullptr };
        int srcLinesize[4] = { width * 4, 0, 0, 0 };

        sws_scale(sws_ctx, srcData, srcLinesize, 0, height,
                  frame->data, frame->linesize);

        frame->pts = pts_counter++;

        // 编码帧
        avcodec_send_frame(codec_ctx, frame);
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
            av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }

    int width, height;
    SwsContext* sws_ctx;
    AVCodecContext* codec_ctx;
    AVFormatContext* fmt_ctx;
    AVStream* stream;
    AVFrame* frame;
    std::vector<uint8_t> captureBuffer;
    int64_t pts_counter = 0;
};

int main() {
    av_log_set_level(AV_LOG_ERROR);
    ScreenRecorder recorder;
    if (recorder.Initialize("output.mp4")) {
        recorder.Record();
    }
    return 0;
}
