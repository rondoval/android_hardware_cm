/*
 * Copyright (C) 2012, rondoval
 * Copyright (C) 2012, Won-Kyu Park
 * Copyright (C) 2012, Raviprasad V Mummidi
 * Copyright (C) 2011, Ivan Zupan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * ChangeLog
 *
 * 2012/01/19 - based on Raviprasad V Mummidi's code and some code by Ivan Zupan
 * 2012/01/21 - cleaned up by wkpark.
 * 2012/02/09 - first working version for P990/SU660 with software rendering
 *            - need to revert "MemoryHeapBase: Save and binderize the offset"
 *              commit f24c4cd0f204068a17f61f1c195ccf140c6c1d67.
 *            - some wrapper functions are needed (please see the libui.patch)
 * 2012/02/19 - Generic cleanup and overlay support (for Milestone 2)
 */

#define LOG_TAG "CameraHAL"
//#define LOG_NDEBUG 0

#include "CameraHardwareInterface.h"
#include <hardware/camera.h>
#include <binder/IMemory.h>
#include <hardware/gralloc.h>
#include <utils/Errors.h>
#include <vector>

using namespace std;

/* Prototypes and extern functions. */
#ifdef BOARD_USE_FROYO_LIBCAMERA
extern "C" android::sp<android::CameraHardwareInterface> openCameraHardware(int cameraId);
#else
extern "C" android::sp<android::CameraHardwareInterface> HAL_openCameraHardware(int cameraId);
extern "C" int HAL_getNumberOfCameras();
extern "C" void HAL_getCameraInfo(int cameraId, struct CameraInfo* cameraInfo);
#endif

namespace android {
    int camera_device_open(const hw_module_t* module, const char* name, hw_device_t** device);
    int CameraHAL_GetCam_Info(int camera_id, struct camera_info *info);
#ifdef BOARD_USE_FROYO_LIBCAMERA
    int CameraHAL_GetNumberOfCameras(void);
#endif
}

static hw_module_methods_t camera_module_methods = {
   open: android::camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
   common: {
      tag: HARDWARE_MODULE_TAG,
      version_major: 1,
      version_minor: 1,
      id: CAMERA_HARDWARE_MODULE_ID,
      name: "Camera HAL for ICS/CM9",
      author: "Won-Kyu Park, Raviprasad V Mummidi, Ivan Zupan, rondoval",
      methods: &camera_module_methods,
      dso: NULL,
      reserved: {0},
   },
#ifdef BOARD_USE_FROYO_LIBCAMERA
   get_number_of_cameras: android::CameraHAL_GetNumberOfCameras,
#else
   get_number_of_cameras: android::HAL_getNumberOfCameras,
#endif
   get_camera_info: android::CameraHAL_GetCam_Info,
};


namespace android {

struct legacy_camera_device {
   camera_device_t device;
   int id;

   // New world
   camera_notify_callback         notify_callback;
   camera_data_callback           data_callback;
   camera_data_timestamp_callback data_timestamp_callback;
   camera_request_memory          request_memory;
   void                          *user;
   preview_stream_ops            *window;

   // Old world
   sp<CameraHardwareInterface>  hwif;
   gralloc_module_t const               *gralloc;
   vector<camera_memory_t*>              sentMem;
   sp<Overlay>                           overlay;

   int32_t                               previewWidth;
   int32_t                               previewHeight;
   OverlayFormats                        previewFormat;
   uint32_t                              previewBpp;
};

/** camera_hw_device implementation **/
static inline struct legacy_camera_device * to_lcdev(struct camera_device *dev) {
    return reinterpret_cast<struct legacy_camera_device *>(dev);
}

static inline void log_camera_params(const char* name, const CameraParameters params) {
    LOGV("%s: dumping camera params", name);
    params.dump();
}

//
// http://code.google.com/p/android/issues/detail?id=823#c4
//
void Yuv420spToRgb565(char* rgb, char* yuv420sp, int width, int height, int stride) {
    int frameSize = width * height;
    int padding = (stride - width) * 2; //two bytes per pixel for rgb565
    int colr = 0;
    for (int j = 0, yp = 0, k = 0; j < height; j++) {
        int uvp = frameSize + (j >> 1) * width, u = 0, v = 0;
        for (int i = 0; i < width; i++, yp++) {
            int y = (0xff & ((int) yuv420sp[yp])) - 16;
            if (y < 0) y = 0;
            if ((i & 1) == 0) {
                v = (0xff & yuv420sp[uvp++]) - 128;
                u = (0xff & yuv420sp[uvp++]) - 128;
            }

            int y1192 = 1192 * y;
            int r = (y1192 + 1634 * v);
            int g = (y1192 - 833 * v - 400 * u);
            int b = (y1192 + 2066 * u);

            if (r < 0) r = 0; else if (r > 262143) r = 262143;
            if (g < 0) g = 0; else if (g > 262143) g = 262143;
            if (b < 0) b = 0; else if (b > 262143) b = 262143;

            /* for RGB565 */
            r = (r >> 13) & 0x1f;
            g = (g >> 12) & 0x3f;
            b = (b >> 13) & 0x1f;

            rgb[k++] = g << 5 | b;
            rgb[k++] = r << 3 | g >> 3;
        }
        k += padding;
    }
}

void Yuv422iToRgb565 (char* rgb, char* yuv422i, int width, int height, int stride) {
    int yuv_index = 0;
    int rgb_index = 0;
    int padding = (stride - width) * 2; //two bytes per pixel for rgb565

    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width / 2; i++) {

            int y1 = (0xff & ((int) yuv422i[yuv_index++])) - 16;
            if (y1 < 0) y1 = 0;

            int u = (0xff & yuv422i[yuv_index++]) - 128;

            int y2 = (0xff & ((int) yuv422i[yuv_index++])) - 16;
            if (y2 < 0) y2 = 0;

            int v = (0xff & yuv422i[yuv_index++]) - 128;

            int y1192 = 1192 * y1;
            int r = (y1192 + 1634 * v);
            int g = (y1192 - 833 * v - 400 * u);
            int b = (y1192 + 2066 * u);

            if (r < 0) r = 0; else if (r > 262143) r = 262143;
            if (g < 0) g = 0; else if (g > 262143) g = 262143;
            if (b < 0) b = 0; else if (b > 262143) b = 262143;

            /* for RGB565 */
            r = (r >> 13) & 0x1f;
            g = (g >> 12) & 0x3f;
            b = (b >> 13) & 0x1f;

            rgb[rgb_index++] = g << 5 | b;
            rgb[rgb_index++] = r << 3 | g >> 3;

            y1192 = 1192 * y2;
            r = (y1192 + 1634 * v);
            g = (y1192 - 833 * v - 400 * u);
            b = (y1192 + 2066 * u);

            if (r < 0) r = 0; else if (r > 262143) r = 262143;
            if (g < 0) g = 0; else if (g > 262143) g = 262143;
            if (b < 0) b = 0; else if (b > 262143) b = 262143;

            /* for RGB565 */
            r = (r >> 13) & 0x1f;
            g = (g >> 12) & 0x3f;
            b = (b >> 13) & 0x1f;

            rgb[rgb_index++] = g << 5 | b;
            rgb[rgb_index++] = r << 3 | g >> 3;
        }
        rgb_index += padding;
    }
}

void CameraHAL_ProcessPreviewData(char *frame, size_t size, legacy_camera_device *lcdev) {
    LOGV("%s: frame=%p, size=%d, camera=%p", __FUNCTION__, frame, size, lcdev);
    if (NULL != lcdev->window) {
        int32_t stride;
        buffer_handle_t *bufHandle = NULL;
        int retVal = lcdev->window->dequeue_buffer(lcdev->window, &bufHandle, &stride);
        if (retVal == NO_ERROR) {
            LOGV("%s: dequeued window, stride=%d", __FUNCTION__, stride);
            if ( stride != lcdev->previewWidth) {
                LOGV("%s: stride=%d doesn't equal width=%d", __FUNCTION__, stride, lcdev->previewWidth);
            }
            retVal = lcdev->window->lock_buffer(lcdev->window, bufHandle);
            if (retVal == NO_ERROR) {
                LOGV("%s: window locked", __FUNCTION__);

                int tries = 5;
                int err = 0;
                void *vaddr;
                err = lcdev->gralloc->lock(lcdev->gralloc, *bufHandle, GRALLOC_USAGE_SW_WRITE_OFTEN,
                            0, 0, lcdev->previewWidth, lcdev->previewHeight, &vaddr);
                while (err && tries) {
                    // Pano frames almost always need a retry...
                    LOGV("%s: gralloc lock retry", __FUNCTION__);
                    usleep(1000);
                    lcdev->gralloc->unlock(lcdev->gralloc, *bufHandle);
                    err = lcdev->gralloc->lock(lcdev->gralloc, *bufHandle, GRALLOC_USAGE_SW_WRITE_OFTEN,
                            0, 0, lcdev->previewWidth, lcdev->previewHeight, &vaddr);
                    tries--;
                }
                if (!err) {
                    // The data we get is in YUV... but Window is RGB565. It needs to be converted
                    switch (lcdev->previewFormat) {
                        case OVERLAY_FORMAT_YUV422I:
                            Yuv422iToRgb565((char*)vaddr, frame, lcdev->previewWidth, lcdev->previewHeight, stride);
                            break;
                        case OVERLAY_FORMAT_YUV420SP:
                            Yuv420spToRgb565((char*)vaddr, frame, lcdev->previewWidth, lcdev->previewHeight, stride);
                            break;
                        case OVERLAY_FORMAT_RGB565:
                            memcpy(vaddr, frame, size);
                            break;
                        default:
                            LOGE("%s: Unknown video format, cannot convert!", __FUNCTION__);
                    }

                    lcdev->gralloc->unlock(lcdev->gralloc, *bufHandle);
                } else {
                    LOGE("%s: could not lock gralloc buffer", __FUNCTION__);
                }
                if (0 != lcdev->window->enqueue_buffer(lcdev->window, bufHandle)) {
                    LOGE("%s: could not enqueue gralloc buffer", __FUNCTION__);
                }
            } else {
                LOGE("%s: ERROR locking the buffer", __FUNCTION__);
                lcdev->window->cancel_buffer(lcdev->window, bufHandle);
            }
         } else {
            LOGE("%s: ERROR dequeueing the buffer", __FUNCTION__);
         }
    }
}

/* Overlay hooks */
void queue_buffer_hook(void *data, void *buffer, size_t size) {
  if (data != NULL && buffer != NULL) {
      CameraHAL_ProcessPreviewData((char*)buffer, size, (legacy_camera_device*) data);
  }
}

void CameraHAL_HandlePreviewData(const sp<IMemory>& dataPtr, void* user) {
   LOGV("%s", __FUNCTION__);
   if (user != NULL) {
       struct legacy_camera_device *lcdev = (struct legacy_camera_device *) user;
       ssize_t  offset;
       size_t   size;
       sp<IMemoryHeap> mHeap = dataPtr->getMemory(&offset, &size);
       char* buffer = (char*)mHeap->getBase() + offset;
       CameraHAL_ProcessPreviewData(buffer, size, lcdev);
  }
}

camera_memory_t* CameraHAL_GenClientData(const sp<IMemory> &dataPtr, legacy_camera_device *lcdev) {
   ssize_t          offset;
   size_t           size;
   camera_memory_t *clientData = NULL;
   sp<IMemoryHeap> mHeap = dataPtr->getMemory(&offset, &size);

   LOGV("CameraHAL_GenClientData: offset:%#x size:%#x base:%p",
        (unsigned)offset, size, mHeap != NULL ? mHeap->base() : 0);

   clientData = lcdev->request_memory(-1, size, 1, lcdev->user);
   if (clientData != NULL) {
      LOGV("%s: clientData=%p clientData->data=%p", __FUNCTION__, clientData, clientData->data);
      memcpy(clientData->data, (char *)(mHeap->base()) + offset, size);
   } else {
      LOGV("CameraHAL_GenClientData: ERROR allocating memory from client");
   }
   return clientData;
}

void CameraHAL_DataCb(int32_t msg_type, const sp<IMemory>& dataPtr, void *user) {
   struct legacy_camera_device *lcdev = (struct legacy_camera_device *) user;

   LOGV("CameraHAL_DataCb: msg_type:%d user:%p", msg_type, user);

   if (lcdev->data_callback != NULL && lcdev->request_memory != NULL) {
      camera_memory_t *mem = CameraHAL_GenClientData(dataPtr, lcdev);
      if (mem != NULL) {
         LOGV("CameraHAL_DataCb: Posting data to client");
         if (msg_type == CAMERA_MSG_VIDEO_FRAME) {
             lcdev->sentMem.push_back(mem);
         }
         lcdev->data_callback(msg_type, mem, 0, NULL, lcdev->user);
      }
   }

   if (msg_type == CAMERA_MSG_PREVIEW_FRAME && lcdev->overlay == NULL) {
      LOGV("CameraHAL_DataCb: preview size = %dx%d", lcdev->previewWidth, lcdev->previewHeight);
      CameraHAL_HandlePreviewData(dataPtr, lcdev);
   }
}

void CameraHAL_DataTSCb(nsecs_t timestamp, int32_t msg_type, const sp<IMemory>& dataPtr, void *user) {
   struct legacy_camera_device *lcdev = (struct legacy_camera_device *) user;

   LOGV("CameraHAL_DataTSCb: timestamp:%lld msg_type:%d user:%p",
        timestamp /1000, msg_type, user);

   if (lcdev->data_timestamp_callback != NULL && lcdev->request_memory != NULL) {
      camera_memory_t *mem = CameraHAL_GenClientData(dataPtr, lcdev);
      if (mem != NULL) {
         LOGV("CameraHAL_DataTSCb: Posting data to client timestamp:%lld", systemTime());
         lcdev->sentMem.push_back(mem);
         lcdev->data_timestamp_callback(timestamp, msg_type, mem, /*index*/0, lcdev->user);
         lcdev->hwif->releaseRecordingFrame(dataPtr);
      } else {
         LOGV("CameraHAL_DataTSCb: ERROR allocating memory from client");
      }
   }
}

/* HAL helper functions. */
void CameraHAL_NotifyCb(int32_t msg_type, int32_t ext1, int32_t ext2, void *user) {
   struct legacy_camera_device *lcdev = (struct legacy_camera_device *) user;

   if (NULL == lcdev) {
     return;
   }

   LOGV("%s: msg_type:%d ext1:%d ext2:%d user:%p", __FUNCTION__, msg_type, ext1, ext2, user);
   if (lcdev->notify_callback != NULL) {
      lcdev->notify_callback(msg_type, ext1, ext2, lcdev->user);
   }
}

#ifdef BOARD_USE_FROYO_LIBCAMERA
int CameraHAL_GetNumberOfCameras(void) {
    LOGV("%s", __FUNCTION__);
    return 1;
}
#endif

int CameraHAL_GetCam_Info(int camera_id, struct camera_info *info) {
   LOGV("%s", __FUNCTION__);
   int rv = 0;

#ifdef BOARD_USE_FROYO_LIBCAMERA
   info->facing = CAMERA_FACING_BACK;
   info->orientation = 90;
#else
   CameraInfo cam_info;
   HAL_getCameraInfo(camera_id, &cam_info);

   info->facing = cam_info.facing;
#ifdef MOTOROLA_CAMERA
   info->orientation = 90; // Milestone2 camera returns 0, but then picture is rotated
#else
   info->orientation = cam_info.orientation;
#endif
#endif //BOARD_USE_FROYO_LIBCAMERA

   LOGD("%s: id:%i faceing:%i orientation: %i", __FUNCTION__, camera_id, info->facing, info->orientation);

   return rv;
}

void CameraHAL_FixupParams(CameraParameters &settings)
{
#ifdef MOTOROLA_CAMERA
  // Milestone2 camera doesn't support YUV420sp... it advertises so, but then sends YUV422I-yuyv data
  settings.set(CameraParameters::KEY_VIDEO_FRAME_FORMAT, CameraParameters::PIXEL_FORMAT_YUV422I);
  settings.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "848x480");
  settings.set(CameraParameters::KEY_PREVIEW_FRAME_RATE, "24");
  settings.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES, "5,10,15,20,24");
  settings.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS, CameraParameters::PIXEL_FORMAT_YUV422I);
  settings.setPreviewFormat(CameraParameters::PIXEL_FORMAT_YUV422I);

  LOGD("Parameters fixed up");
#endif
}

inline void destroyOverlay(legacy_camera_device *lcdev) {
  if (lcdev->overlay != NULL) {
      lcdev->hwif->setOverlay(NULL);
      lcdev->overlay = NULL;
  }
}

/* Hardware Camera interface handlers. */
int camera_set_preview_window(struct camera_device * device, struct preview_stream_ops *window) {
  int rv = -EINVAL;
  struct legacy_camera_device *lcdev = to_lcdev(device);

  LOGV("camera_set_preview_window : Window:%p", window);
  if (device == NULL) {
      LOGE("camera_set_preview_window: Invalid device.");
      return -EINVAL;
  }

  if (lcdev->window == window) {
      // reconfigure the old window, preview size might have changed
      LOGV("%s: reconfiguring window", __FUNCTION__);
      destroyOverlay(lcdev);
  }

  lcdev->window = window;
  if (window == NULL) {
      // It means we need to release old window
      LOGV("%s: releasing previous window", __FUNCTION__);
      destroyOverlay(lcdev);
      return NO_ERROR;
  }
  LOGV("%s : OK window is %p", __FUNCTION__, window);

  if (!lcdev->gralloc) {
      hw_module_t const* module;
      int err = 0;
      if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
          lcdev->gralloc = (const gralloc_module_t *)module;
          LOGD("%s: loaded gralloc, module name=%s; author=%s", __FUNCTION__, module->name, module->author);
      } else {
          LOGE("%s: Fail on loading gralloc HAL", __FUNCTION__);
      }
  }
  LOGV("%s: OK on loading gralloc HAL", __FUNCTION__);

  int min_bufs = -1;
  if (window->get_min_undequeued_buffer_count(window, &min_bufs)) {
      LOGE("%s: could not retrieve min undequeued buffer count", __FUNCTION__);
      return -1;
  }

  LOGV("%s: min bufs:%i", __FUNCTION__, min_bufs);

  int kBufferCount = min_bufs + 2;
  LOGV("%s: setting buffer count to %i", __FUNCTION__, kBufferCount);
  if (window->set_buffer_count(window, kBufferCount)) {
      LOGE("%s: could not set buffer count", __FUNCTION__);
      return -1;
  }

  CameraParameters params(lcdev->hwif->getParameters());
  params.getPreviewSize(&lcdev->previewWidth, &lcdev->previewHeight);
  int hal_pixel_format = HAL_PIXEL_FORMAT_RGB_565;

  const char *str_preview_format = params.getPreviewFormat();
  LOGD("%s: preview format %s", __FUNCTION__, str_preview_format);
  lcdev->previewFormat = getOverlayFormatFromString(str_preview_format);
  lcdev->previewBpp = getBppFromOverlayFormat(lcdev->previewFormat);

  if (window->set_usage(window, GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN)) {
      LOGE("%s: could not set usage on gralloc buffer", __FUNCTION__);
      return -1;
  }

  if (window->set_buffers_geometry(window, lcdev->previewWidth, lcdev->previewHeight, hal_pixel_format)) {
      LOGE("%s: could not set buffers geometry", __FUNCTION__);
      return -1;
  }

  if (lcdev->hwif->useOverlay()) {
     LOGI("%s: Using overlay for device %p", __FUNCTION__, lcdev);
     lcdev->overlay= new Overlay(lcdev->previewWidth, lcdev->previewHeight,  lcdev->previewFormat, queue_buffer_hook, (void*) lcdev);
     lcdev->hwif->setOverlay(lcdev->overlay);
  }

  return NO_ERROR;
}

void camera_set_callbacks(struct camera_device * device,
                      camera_notify_callback notify_cb,
                      camera_data_callback data_cb,
                      camera_data_timestamp_callback data_cb_timestamp,
                      camera_request_memory get_memory, void *user)
{
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_set_callbacks: notify_cb: %p, data_cb: %p data_cb_timestamp: %p, get_memory: %p, user :%p",
        notify_cb, data_cb, data_cb_timestamp, get_memory, user);

   lcdev->notify_callback = notify_cb;
   lcdev->data_callback = data_cb;
   lcdev->data_timestamp_callback = data_cb_timestamp;
   lcdev->request_memory = get_memory;
   lcdev->user = user;

   lcdev->hwif->setCallbacks(CameraHAL_NotifyCb, CameraHAL_DataCb, CameraHAL_DataTSCb, (void *) lcdev);
}

void camera_enable_msg_type(struct camera_device * device, int32_t msg_type) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_enable_msg_type: msg_type:%d\n", msg_type);
   lcdev->hwif->enableMsgType(msg_type);
}

void camera_disable_msg_type(struct camera_device * device, int32_t msg_type) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_disable_msg_type: msg_type:%d\n", msg_type);
   if (msg_type == CAMERA_MSG_VIDEO_FRAME) {
       LOGV("%s: releasing stale video frames", __FUNCTION__);
       for(vector<camera_memory_t*>::iterator it = lcdev->sentMem.begin(); it != lcdev->sentMem.end(); ++it) {
           (*it)->release(*it);
       }
       lcdev->sentMem.clear();
   }
   lcdev->hwif->disableMsgType(msg_type);
}

int camera_msg_type_enabled(struct camera_device * device, int32_t msg_type) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_msg_type_enabled: msg_type:%d\n", msg_type);
   return lcdev->hwif->msgTypeEnabled(msg_type);
}

int camera_start_preview(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_start_preview:\n");
   return lcdev->hwif->startPreview();
}

void camera_stop_preview(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_stop_preview:\n");
   lcdev->hwif->stopPreview();
   return;
}

int camera_preview_enabled(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   int ret = lcdev->hwif->previewEnabled();
   LOGV("camera_preview_enabled: %d\n", ret);
   return ret;
}

int camera_store_meta_data_in_buffers(struct camera_device * device, int enable) {
   LOGV("camera_store_meta_data_in_buffers:\n");
   return INVALID_OPERATION;
}

int camera_start_recording(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_start_recording\n");
   lcdev->hwif->startRecording();
   return NO_ERROR;
}

void camera_stop_recording(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_stop_recording:\n");
   lcdev->hwif->stopRecording();
}

int camera_recording_enabled(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_recording_enabled:\n");
   return (int)lcdev->hwif->recordingEnabled();
}

void camera_release_recording_frame(struct camera_device * device, const void *opaque) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_release_recording_frame: opaque: %p", opaque);
   if (opaque != NULL) {
       for(vector<camera_memory_t*>::iterator it = lcdev->sentMem.begin(); it != lcdev->sentMem.end(); ++it) {
           camera_memory_t *mem = *it;
           if (mem->data == opaque) {
               LOGV("found, removing");
               mem->release(mem);
               lcdev->sentMem.erase(it);
               break;
           }
       }
   }
}

int camera_auto_focus(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_auto_focus:\n");
   lcdev->hwif->autoFocus();
   return NO_ERROR;
}

int camera_cancel_auto_focus(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_cancel_auto_focus:\n");
   lcdev->hwif->cancelAutoFocus();
   return NO_ERROR;
}

int camera_take_picture(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_take_picture:\n");
   lcdev->hwif->takePicture();
   return NO_ERROR;
}

int camera_cancel_picture(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_cancel_picture:\n");
   lcdev->hwif->cancelPicture();
   return NO_ERROR;
}

int camera_set_parameters(struct camera_device * device, const char *params) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   String8 s(params);
   CameraParameters p(s);
   log_camera_params(__FUNCTION__, s);
   lcdev->hwif->setParameters(p);
   return NO_ERROR;
}

char* camera_get_parameters(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   char *rc = NULL;
   CameraParameters params(lcdev->hwif->getParameters());
   CameraHAL_FixupParams(params);
   log_camera_params(__FUNCTION__, params);
   rc = strdup((char *)params.flatten().string());
   return rc;
}

void camera_put_parameters(struct camera_device *device, char *params) {
  if (params != NULL) {
      free(params);
  }
}

int camera_send_command(struct camera_device * device, int32_t cmd, int32_t arg0, int32_t arg1) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_send_command: cmd:%d arg0:%d arg1:%d\n", cmd, arg0, arg1);
   return lcdev->hwif->sendCommand(cmd, arg0, arg1);
}

void camera_release(struct camera_device * device) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_release:\n");
   destroyOverlay(lcdev);
   lcdev->hwif->release();
}

int camera_dump(struct camera_device * device, int fd) {
   struct legacy_camera_device *lcdev = to_lcdev(device);
   LOGV("camera_dump:\n");
   Vector<String16> args;
   return lcdev->hwif->dump(fd, args);
}

int camera_device_close(hw_device_t* device) {
   struct camera_device * hwdev = reinterpret_cast<struct camera_device *>(device);
   struct legacy_camera_device *lcdev = to_lcdev(hwdev);
   int rc = -EINVAL;
   LOGD("camera_device_close\n");
   if (lcdev != NULL) {
      camera_device_ops_t *camera_ops = lcdev->device.ops;
      if (camera_ops) {
         if (lcdev->hwif != NULL) {
            lcdev->hwif.clear();
         }
         free(camera_ops);
      }
      free(lcdev);
      rc = NO_ERROR;
   }
   return rc;
}

int camera_device_open(const hw_module_t* module, const char* name, hw_device_t** device) {
   int ret;
   struct legacy_camera_device *lcdev;
   camera_device_t* camera_device;
   camera_device_ops_t* camera_ops;

   if (NULL == name) {
      return 0;
   }

   int cameraId = atoi(name);

   LOGD("%s: name:%s device:%p cameraId:%d\n", __FUNCTION__, name, device, cameraId);

   lcdev = (struct legacy_camera_device *)calloc(1, sizeof(*lcdev));
   //TODO if lcdev = null -ENOMEM
   camera_ops = (camera_device_ops_t *)malloc(sizeof(*camera_ops));
   memset(camera_ops, 0, sizeof(*camera_ops));

   lcdev->device.common.tag               = HARDWARE_DEVICE_TAG;
   lcdev->device.common.version           = 0;
   lcdev->device.common.module            = (hw_module_t *)(module);
   lcdev->device.common.close             = camera_device_close;
   lcdev->device.ops                      = camera_ops;

   camera_ops->set_preview_window         = camera_set_preview_window;
   camera_ops->set_callbacks              = camera_set_callbacks;
   camera_ops->enable_msg_type            = camera_enable_msg_type;
   camera_ops->disable_msg_type           = camera_disable_msg_type;
   camera_ops->msg_type_enabled           = camera_msg_type_enabled;
   camera_ops->start_preview              = camera_start_preview;
   camera_ops->stop_preview               = camera_stop_preview;
   camera_ops->preview_enabled            = camera_preview_enabled;
   camera_ops->store_meta_data_in_buffers = camera_store_meta_data_in_buffers;
   camera_ops->start_recording            = camera_start_recording;
   camera_ops->stop_recording             = camera_stop_recording;
   camera_ops->recording_enabled          = camera_recording_enabled;
   camera_ops->release_recording_frame    = camera_release_recording_frame;
   camera_ops->auto_focus                 = camera_auto_focus;
   camera_ops->cancel_auto_focus          = camera_cancel_auto_focus;
   camera_ops->take_picture               = camera_take_picture;
   camera_ops->cancel_picture             = camera_cancel_picture;

   camera_ops->set_parameters             = camera_set_parameters;
   camera_ops->get_parameters             = camera_get_parameters;
   camera_ops->put_parameters             = camera_put_parameters;
   camera_ops->send_command               = camera_send_command;
   camera_ops->release                    = camera_release;
   camera_ops->dump                       = camera_dump;

   lcdev->id = cameraId;
#ifdef BOARD_USE_FROYO_LIBCAMERA
   lcdev->hwif = openCameraHardware(cameraId);
#else
   lcdev->hwif = HAL_openCameraHardware(cameraId);
#endif
   if (lcdev->hwif == NULL) {
       ret = -EIO;
       goto err_create_camera_hw;
   }
   *device = &lcdev->device.common;
   return NO_ERROR;

err_create_camera_hw:
   free(lcdev);
   free(camera_ops);
   return ret;
}

} /* namespace android */
