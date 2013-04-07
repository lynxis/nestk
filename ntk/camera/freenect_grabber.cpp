/**
 * Copyright (C) 2013 ManCTL SARL <contact@manctl.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Nicolas Burrus <nicolas.burrus@manctl.com>
 */


#include "freenect_grabber.h"
extern "C" {
#include <libfreenect.h>
}
#include <ntk/utils/opencv_utils.h>
#include <ntk/utils/time.h>
#include <ntk/camera/rgbd_processor.h>
#include <ntk/camera/rgbd_image.h>

// FIXME: Factor this out.
#ifdef _WIN32
#   define NOMINMAX
#   define VC_EXTRALEAN
#   define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#   undef WIN32_LEAN_AND_MEAN
#   undef VC_EXTRALEAN
#   undef NOMINMAX
#endif

using namespace cv;

namespace ntk
{

static void kinect_depth_db(freenect_device *dev, void *v_depth, uint32_t timestamp)
{
    FreenectGrabber* grabber = reinterpret_cast<FreenectGrabber*>(freenect_get_user(dev));
    uint16_t *depth = reinterpret_cast<uint16_t*>(v_depth);
    grabber->depthCallBack(depth);
}

static void kinect_video_db(freenect_device *dev, void *rgb, uint32_t timestamp)
{
    FreenectGrabber* grabber = reinterpret_cast<FreenectGrabber*>(freenect_get_user(dev));
    if (grabber->irModeEnabled()) { // ir mode
		uint8_t *ir_cast = reinterpret_cast<uint8_t*>(rgb);
		grabber->irCallBack(ir_cast);
	} else { // rgb mode
	    uint8_t *rgb_cast = reinterpret_cast<uint8_t*>(rgb);
	    grabber->rgbCallBack(rgb_cast);
	}
}

void FreenectGrabber :: irCallBack(uint8_t *buf)
{
//    ntk_assert(f_video_mode.width == m_current_image.rawIntensity().cols, "Bad width");
//    ntk_assert(f_video_mode.height == m_current_image.rawIntensity().rows, "Bad height");
    int width = 640;
    int height = 480;
    float* intensity_buf = m_current_image.rawIntensityRef().ptr<float>();
    for (int i = 0; i < width*height; ++i)
        *intensity_buf++ = *buf++;
    m_rgb_transmitted = false;
}

void FreenectGrabber :: depthCallBack(uint16_t *buf)
{
//    ntk_assert(f_depth_mode.width == m_current_image.rawDepth().cols, "Bad width");
//    ntk_assert(f_depth_mode.height == m_current_image.rawDepth().rows, "Bad height");
    int width = f_depth_mode.width;
    int height = f_depth_mode.height;
    float* depth_buf = m_current_image.rawDepthRef().ptr<float>();
    for (int i = 0; i < width*height; ++i)
        *depth_buf++ = *buf++;
    m_depth_transmitted = false;
}

void FreenectGrabber :: rgbCallBack(uint8_t *buf)
{
//    ntk_assert(f_video_mode.width == m_current_image.rawRgb().cols, "Bad width");
//    ntk_assert(f_video_mode.height == m_current_image.rawRgb().rows, "Bad height");
    int width = 640;
    int height = 480;
    std::copy(buf, buf+3*width*height, (uint8_t*)m_current_image.rawRgbRef().ptr());
    cvtColor(m_current_image.rawRgb(), m_current_image.rawRgbRef(), CV_RGB2BGR);
    m_rgb_transmitted = false;
}

void FreenectGrabber :: setTiltAngle(int angle)
{
    if (!isConnected()) return;
    freenect_set_tilt_degs(f_dev, angle);
}

void FreenectGrabber :: setDualRgbIR(bool enable)
{
    m_dual_ir_rgb = enable;
}

void FreenectGrabber :: setIRMode(bool ir)
{
    if (!isConnected()) return;

    QWriteLocker locker(&m_lock);
    m_ir_mode = ir;
    freenect_stop_video(f_dev);
    if (!m_ir_mode)
        setVideoMode(freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_RGB));
    else
        setVideoMode(freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_IR_8BIT));
    freenect_start_video(f_dev);
}

void FreenectGrabber :: startKinect()
{
    freenect_start_depth(f_dev);
    setIRMode(m_ir_mode);
}

bool FreenectGrabber :: connectToDevice()
{
    if (m_connected)
        return true;

    if (freenect_init(&f_ctx, NULL) < 0)
    {
        ntk_dbg(0) << "freenect_init() failed";
        return false;
    }

    freenect_set_log_level(f_ctx, FREENECT_LOG_DEBUG);
    freenect_select_subdevices(f_ctx, (freenect_device_flags)(FREENECT_DEVICE_MOTOR | FREENECT_DEVICE_CAMERA));

    int nr_devices = freenect_num_devices (f_ctx);
    ntk_dbg(0) << "freenect found " << nr_devices << " kinect devices\n";

    ntk_dbg(0) << "Connecting to device: " << m_device_id;
    if (freenect_open_device(f_ctx, &f_dev, m_device_id) < 0)
    {
        ntk_dbg(0) << "freenect_open_device() failed";
        return false;
    }

    freenect_set_user(f_dev, this);

    setVideoMode(freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_RGB));
    setDepthMode(freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_DEPTH_11BIT));

    freenect_set_depth_callback(f_dev, kinect_depth_db);
    freenect_set_video_callback(f_dev, kinect_video_db);

    this->setIRMode(m_ir_mode);
    m_connected = true;
    return true;
}

bool FreenectGrabber :: disconnectFromDevice()
{
    // Exit requested.
    m_connected = false;
    freenect_close_device(f_dev);
    freenect_shutdown(f_ctx);
    return true;
}

void FreenectGrabber :: run()
{
    setThreadShouldExit(false);
    m_current_image.setCalibration(m_calib_data);
    m_rgbd_image.setCalibration(m_calib_data);

    m_rgbd_image.rawRgbRef() = Mat3b(f_video_mode.height, f_video_mode.width);
    m_rgbd_image.rawDepthRef() = Mat1f(f_video_mode.height, f_video_mode.width);
    m_rgbd_image.rawIntensityRef() = Mat1f(f_video_mode.height, f_video_mode.width);

    m_current_image.rawRgbRef() = Mat3b(f_video_mode.height, f_video_mode.width);
    m_current_image.rawDepthRef() = Mat1f(f_video_mode.height, f_video_mode.width);
    m_current_image.rawIntensityRef() = Mat1f(f_video_mode.height, f_video_mode.width);

    startKinect();
    int64 last_grab_time = 0;

    while (!threadShouldExit())
    {
        waitForNewEvent(-1); // Use infinite timeout in order to honor sync mode.

        while (m_depth_transmitted || m_rgb_transmitted)
            freenect_process_events(f_ctx);

        // m_current_image.rawDepth().copyTo(m_current_image.rawAmplitudeRef());
        // m_current_image.rawDepth().copyTo(m_current_image.rawIntensityRef());

        {
            int64 grab_time = ntk::Time::getMillisecondCounter();
            ntk_dbg_print(grab_time - last_grab_time, 2);
            last_grab_time = grab_time;
            QWriteLocker locker(&m_lock);
            // FIXME: ugly hack to handle the possible time
            // gaps between rgb and IR frames in dual mode.
            if (m_dual_ir_rgb)
                m_current_image.copyTo(m_rgbd_image);
            else
                m_current_image.copyTo(m_rgbd_image);
            m_rgb_transmitted = true;
            m_depth_transmitted = true;
        }

        if (m_dual_ir_rgb)
            setIRMode(!m_ir_mode);
//        cv::Mat3b scaled_image;
//        scaled_image = m_rgbd_image.rawRgb();
//        cv::imwrite("/tmp/image_grabber.png", scaled_image);
        advertiseNewFrame();
#ifdef _WIN32
        // FIXME: this is to avoid GUI freezes with libfreenect on Windows.
        // See http://groups.google.com/group/openkinect/t/b1d828d108e9e69
        Sleep(1);
#endif
    }
}

void FreenectGrabber :: setDepthMode(freenect_frame_mode mode)
{
    f_depth_mode = mode;
    freenect_set_depth_mode(f_dev, mode);
}

void FreenectGrabber :: setVideoMode(freenect_frame_mode mode)
{
    f_video_mode = mode;
    freenect_set_video_mode(f_dev, mode);
}

} // ntk
