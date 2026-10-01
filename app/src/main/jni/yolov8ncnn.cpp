// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <android/asset_manager_jni.h>
#include <android/native_window_jni.h>
#include <android/native_window.h>

#include <android/log.h>

#include <jni.h>

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <platform.h>
#include <benchmark.h>

#include "yolov8.h"

#include "ndkcamera.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#if __ARM_NEON
#include <arm_neon.h>
#endif // __ARM_NEON

static YOLOv8* g_yolov8 = 0;
static ncnn::Mutex lock;

// 最近一帧结果，供 Java 侧轮询读取
static std::string g_result_string;
static std::vector<uint8_t> g_result_frame;

// 最近一帧 RGB 画面，供 WebUI 视频流使用
static std::vector<uint8_t> g_frame_rgb;
static int g_frame_w = 0;
static int g_frame_h = 0;
static bool g_draw_boxes = false;  // 是否在视频流上叠加检测框

// ===== 标定参数（TODO：按一加9Pro实际相机与安装位标定）=====
static const float CAM_HEIGHT_M = 1.2f;  // 相机离地高度(米)
static const float CAM_FX_PX = 1000.f;   // 焦距(像素)

static uint8_t coco_to_class(int label)
{
    switch (label)
    {
        case 0:  return 0x01; // person
        case 1:  return 0x02; // bicycle
        case 2:  return 0x03; // car
        case 3:  return 0x04; // motorcycle
        case 5:  return 0x05; // bus
        case 7:  return 0x06; // truck
        case 9:  return 0x07; // traffic light
        case 11: return 0x08; // stop sign
        default: return 0x00; // unknown
    }
}

static uint8_t crc8(const uint8_t* data, size_t n)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

static void process_frame(const cv::Mat& rgb)
{
    std::vector<Object> objects;
    g_yolov8->detect(rgb, objects);

    // 缓存 RGB 画面（供 WebUI 视频流）；可选叠加检测框
    cv::Mat display = rgb;
    if (g_draw_boxes)
    {
        display = rgb.clone();
        g_yolov8->draw(display, objects);
    }
    {
        size_t sz = display.total() * display.elemSize();
        if (display.isContinuous() && sz > 0)
        {
            g_frame_rgb.resize(sz);
            memcpy(g_frame_rgb.data(), display.data, sz);
            g_frame_w = display.cols;
            g_frame_h = display.rows;
        }
    }

    const int img_w = rgb.cols;
    const int img_h = rgb.rows;
    const float cx = img_w * 0.5f;
    const float cy = img_h * 0.5f;

    std::vector<uint8_t> frame;
    // 帧头 10 字节
    frame.push_back(0xAA);
    frame.push_back(0x55);
    frame.push_back(0x01); // version
    frame.push_back(0x01); // type = 检测结果
    static uint8_t seq = 0;
    uint8_t s = seq++;
    frame.push_back(s);
    frame.push_back((uint8_t)objects.size());
    uint32_t ts = (uint32_t)ncnn::get_current_time();
    frame.push_back(ts & 0xFF);
    frame.push_back((ts >> 8) & 0xFF);
    frame.push_back((ts >> 16) & 0xFF);
    frame.push_back((ts >> 24) & 0xFF);

    char text[4096];
    int off = snprintf(text, sizeof(text), "frame#%u n=%d\n", (unsigned)s, (int)objects.size());

    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object& o = objects[i];

        uint8_t cls = coco_to_class(o.label);

        // 底边法：纵向距离(米)
        float y_bottom = o.rect.y + o.rect.height;
        float dist_m = CAM_HEIGHT_M * CAM_FX_PX / (y_bottom - cy);
        uint16_t distance_cm;
        if (dist_m <= 0.f)        distance_cm = 0;
        else if (dist_m >= 655.f) distance_cm = 0xFFFF;
        else                      distance_cm = (uint16_t)(dist_m * 100.f);

        // 水平方位角
        float bx = o.rect.x + o.rect.width * 0.5f;
        float bearing_deg = atanf((bx - cx) / CAM_FX_PX) * 180.f / 3.14159265f;
        int16_t bearing_001 = (int16_t)(bearing_deg * 100.f);

        // 朝向(宽高比启发式，粗略；TODO：换朝向分类器)
        float aspect = o.rect.width / o.rect.height;
        int16_t heading_001 = 0;
        if (aspect > 1.8f)      heading_001 = 9000; // 侧面
        else if (aspect > 1.4f) heading_001 = 4500; // 斜向

        // 对象 8 字节
        frame.push_back(0); // track_id 暂为 0
        frame.push_back(cls);
        frame.push_back(distance_cm & 0xFF);
        frame.push_back((distance_cm >> 8) & 0xFF);
        frame.push_back(bearing_001 & 0xFF);
        frame.push_back((bearing_001 >> 8) & 0xFF);
        frame.push_back(heading_001 & 0xFF);
        frame.push_back((heading_001 >> 8) & 0xFF);

        if (off < (int)sizeof(text) - 64)
            off += snprintf(text + off, sizeof(text) - off,
                            "  #%d cls=0x%02X d=%4.1fm b=%+5.1f h=%+d\n",
                            (int)i, cls, dist_m, bearing_deg, heading_001 / 100);
    }

    // crc8
    frame.push_back(crc8(frame.data(), frame.size()));

    // logcat 打印 hex
    {
        char hex[2048];
        int hoff = 0;
        for (size_t i = 0; i < frame.size() && hoff < (int)sizeof(hex) - 4; i++)
            hoff += snprintf(hex + hoff, sizeof(hex) - hoff, "%02X ", frame[i]);
        __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "frame[%uB] %s", (unsigned)frame.size(), hex);
    }

    // 存结果供 Java 轮询
    g_result_string = text;
    g_result_frame = frame;
}

class MyNdkCamera : public NdkCamera
{
public:
    virtual void on_image(const cv::Mat& rgb) const;
};

void MyNdkCamera::on_image(const cv::Mat& rgb) const
{
    ncnn::MutexLockGuard g(lock);

    if (g_yolov8)
        process_frame(rgb);
}

static MyNdkCamera* g_camera = 0;

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "JNI_OnLoad");

    g_camera = new MyNdkCamera;

    ncnn::create_gpu_instance();

    return JNI_VERSION_1_4;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "JNI_OnUnload");

    {
        ncnn::MutexLockGuard g(lock);

        delete g_yolov8;
        g_yolov8 = 0;
    }

    ncnn::destroy_gpu_instance();

    delete g_camera;
    g_camera = 0;
}

// public native boolean loadModel(AssetManager mgr, int taskid, int modelid, int cpugpu);
JNIEXPORT jboolean JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_loadModel(JNIEnv* env, jobject thiz, jobject assetManager, jint taskid, jint modelid, jint cpugpu)
{
    if (taskid < 0 || taskid > 5 || modelid < 0 || modelid > 8 || cpugpu < 0 || cpugpu > 2)
    {
        return JNI_FALSE;
    }

    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);

    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "loadModel %p", mgr);

    const char* tasknames[6] =
    {
        "",
        "_oiv7",
        "_seg",
        "_pose",
        "_cls",
        "_obb"
    };

    const char* modeltypes[9] =
    {
        "n",
        "s",
        "m",
        "n",
        "s",
        "m",
        "n",
        "s",
        "m"
    };

    std::string parampath = std::string("yolov8") + modeltypes[(int)modelid] + tasknames[(int)taskid] + ".ncnn.param";
    std::string modelpath = std::string("yolov8") + modeltypes[(int)modelid] + tasknames[(int)taskid] + ".ncnn.bin";
    bool use_gpu = (int)cpugpu == 1;
    bool use_turnip = (int)cpugpu == 2;

    // reload
    {
        ncnn::MutexLockGuard g(lock);

        {
            static int old_taskid = 0;
            static int old_modelid = 0;
            static int old_cpugpu = 0;
            if (taskid != old_taskid || (modelid % 3) != old_modelid || cpugpu != old_cpugpu)
            {
                // taskid or model or cpugpu changed
                delete g_yolov8;
                g_yolov8 = 0;
            }
            old_taskid = taskid;
            old_modelid = modelid % 3;
            old_cpugpu = cpugpu;

            ncnn::destroy_gpu_instance();

            if (use_turnip)
            {
                ncnn::create_gpu_instance("libvulkan_freedreno.so");
            }
            else if (use_gpu)
            {
                ncnn::create_gpu_instance();
            }

            if (!g_yolov8)
            {
                if (taskid == 0) g_yolov8 = new YOLOv8_det_coco;
                if (taskid == 1) g_yolov8 = new YOLOv8_det_oiv7;
                if (taskid == 2) g_yolov8 = new YOLOv8_seg;
                if (taskid == 3) g_yolov8 = new YOLOv8_pose;
                if (taskid == 4) g_yolov8 = new YOLOv8_cls;
                if (taskid == 5) g_yolov8 = new YOLOv8_obb;

                g_yolov8->load(mgr, parampath.c_str(), modelpath.c_str(), use_gpu || use_turnip);
            }
            int target_size = 320;
            if ((int)modelid >= 3)
                target_size = 480;
            if ((int)modelid >= 6)
                target_size = 640;
            g_yolov8->set_det_target_size(target_size);
        }
    }

    return JNI_TRUE;
}

// public native boolean openCamera(int facing);
JNIEXPORT jboolean JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_openCamera(JNIEnv* env, jobject thiz, jint facing)
{
    if (facing < 0 || facing > 1)
        return JNI_FALSE;

    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "openCamera %d", facing);

    g_camera->open((int)facing);

    return JNI_TRUE;
}

// public native boolean closeCamera();
JNIEXPORT jboolean JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_closeCamera(JNIEnv* env, jobject thiz)
{
    __android_log_print(ANDROID_LOG_DEBUG, "ncnn", "closeCamera");

    g_camera->close();

    return JNI_TRUE;
}

// public native String getResultString();
JNIEXPORT jstring JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_getResultString(JNIEnv* env, jobject thiz)
{
    std::string s;
    {
        ncnn::MutexLockGuard g(lock);
        s = g_result_string;
    }
    return env->NewStringUTF(s.c_str());
}

// public native byte[] getFrame();
JNIEXPORT jbyteArray JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_getFrame(JNIEnv* env, jobject thiz)
{
    std::vector<uint8_t> f;
    {
        ncnn::MutexLockGuard g(lock);
        f = g_result_frame;
    }
    jbyteArray arr = env->NewByteArray((jsize)f.size());
    if (f.size() > 0)
        env->SetByteArrayRegion(arr, 0, (jsize)f.size(), (const jbyte*)f.data());
    return arr;
}

// public native byte[] getFrameRGB();
JNIEXPORT jbyteArray JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_getFrameRGB(JNIEnv* env, jobject thiz)
{
    std::vector<uint8_t> buf;
    {
        ncnn::MutexLockGuard g(lock);
        buf = g_frame_rgb;
    }
    jbyteArray arr = env->NewByteArray((jsize)buf.size());
    if (!buf.empty())
        env->SetByteArrayRegion(arr, 0, (jsize)buf.size(), (const jbyte*)buf.data());
    return arr;
}

// public native int getFrameWidth();
JNIEXPORT jint JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_getFrameWidth(JNIEnv* env, jobject thiz)
{
    return g_frame_w;
}

// public native int getFrameHeight();
JNIEXPORT jint JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_getFrameHeight(JNIEnv* env, jobject thiz)
{
    return g_frame_h;
}

// public native int setAFMode(int mode);
JNIEXPORT jint JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_setAFMode(JNIEnv* env, jobject thiz, jint mode)
{
    return g_camera->setAFMode(mode);
}

// public native int setFocusDistance(float diopters);
JNIEXPORT jint JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_setFocusDistance(JNIEnv* env, jobject thiz, jfloat diopters)
{
    return g_camera->setFocusDistance(diopters);
}

// public native int setAEMode(int mode);
JNIEXPORT jint JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_setAEMode(JNIEnv* env, jobject thiz, jint mode)
{
    return g_camera->setAEMode(mode);
}

// public native int setExposureTime(long ns);
JNIEXPORT jint JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_setExposureTime(JNIEnv* env, jobject thiz, jlong ns)
{
    return g_camera->setExposureTime((int64_t)ns);
}

// public native int setSensitivity(int iso);
JNIEXPORT jint JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_setSensitivity(JNIEnv* env, jobject thiz, jint iso)
{
    return g_camera->setSensitivity(iso);
}

// public native int setAwbMode(int mode);
JNIEXPORT jint JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_setAwbMode(JNIEnv* env, jobject thiz, jint mode)
{
    return g_camera->setAwbMode(mode);
}

// public native void setDrawBoxes(boolean on);
JNIEXPORT void JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_setDrawBoxes(JNIEnv* env, jobject thiz, jboolean on)
{
    g_draw_boxes = (on == JNI_TRUE);
}

// public native String listCameras(int facing);
JNIEXPORT jstring JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_listCameras(JNIEnv* env, jobject thiz, jint facing)
{
    return env->NewStringUTF(g_camera->listCameras(facing).c_str());
}

// public native boolean openCameraIndex(int facing, int index);
JNIEXPORT jboolean JNICALL Java_com_tencent_yolov8ncnn_YOLOv8Ncnn_openCameraIndex(JNIEnv* env, jobject thiz, jint facing, jint index)
{
    g_camera->close();
    int r = g_camera->open(facing, index);
    return r == 0 ? JNI_TRUE : JNI_FALSE;
}

}
