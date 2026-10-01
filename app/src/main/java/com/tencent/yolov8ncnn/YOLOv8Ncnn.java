package com.tencent.yolov8ncnn;

import android.content.res.AssetManager;

public class YOLOv8Ncnn
{
    public native boolean loadModel(AssetManager mgr, int taskid, int modelid, int cpugpu);
    public native boolean openCamera(int facing);
    public native boolean closeCamera();
    public native String getResultString();
    public native byte[] getFrame();

    // 视频流
    public native byte[] getFrameRGB();
    public native int getFrameWidth();
    public native int getFrameHeight();
    public native void setDrawBoxes(boolean on);

    // 相机控制
    public native int setAFMode(int mode);
    public native int setFocusDistance(float diopters);
    public native int setAEMode(int mode);
    public native int setExposureTime(long ns);
    public native int setSensitivity(int iso);
    public native int setAwbMode(int mode);

    // 相机枚举与切换
    public native String listCameras(int facing);
    public native boolean openCameraIndex(int facing, int index);

    static {
        System.loadLibrary("yolov8ncnn");
    }
}
