#include <cstdio>
#include <cstdlib>
#include <cstring>      // strcmp
#include <iostream>     // cout
#include <signal.h>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "log.h"
#include "tools.h"
#include "libvMI.h"

#ifdef _WIN32

#define VSNPRINTF(a,...)   vsnprintf(a, (sizeof(a)/sizeof(char)), __VA_ARGS__)
#define SNPRINTF(a,...)    _snprintf_s(a, (sizeof(a)/sizeof(char)), _TRUNCATE, __VA_ARGS__)
#define STRCPY(a,b)        strcpy_s(a, (sizeof(a)/sizeof(char)), b)
#define STRNCPY(a,b,c)     strncpy_s(a, (sizeof(a)/sizeof(char)), b, c)
#define STRCAT(a,b)        strcat_s(a, (sizeof(a)/sizeof(char)), b)

#else   // _WIN32

#define VSNPRINTF(a,...)    vsnprintf(a, (sizeof(a)/sizeof(char)), __VA_ARGS__)
#define SNPRINTF(a,...)     snprintf(a, (sizeof(a)/sizeof(char)), __VA_ARGS__)
#define STRCPY              strcpy
#define STRNCPY             strncpy
#define STRCAT              strcat

#endif  // _WIN32

using namespace std;

/*
 * Some defines...
 */
#define MSG_MAX_LEN     1024

/*
 * Global variables
 */
libvMI_module_handle     g_vMIModule = LIBVMI_INVALID_HANDLE;
std::condition_variable  g_var;
std::mutex               g_mtx;
int                      g_nbFrame = 0;


/**
* Description: signal handler to exit properly
* @method signal_handler
* @param int signum trapping signal
* @return
*/
void signal_handler(int signum) {

    LOG_INFO("Got signal, exiting cleanly...");
    std::unique_lock<std::mutex> lock(g_mtx);
    g_var.notify_all();
    lock.unlock();
}


/*
* Description: Callback used by libvMI to communicate with us
* @method libvMI_callback
* @param const void* user_data Some user defined value (if any). Null if not used.
* @param CmdType cmd Command type (defined on ip2vf.h)
* @param int param (some values returned by libip2vf, not used for now)
* @param libvMI_pin_handle in handle of the pin providing cmd, LIBVMI_INVALID_HANDLE if none
* @param libvMI_frame_handle hFrame handle of the vMI frame provided. LIBVMI_INVALID_HANDLE if not relevant.
* @return
*/
void libvMI_callback(const void* user_data, CmdType cmd, int param, libvMI_pin_handle in, libvMI_frame_handle hFrame)
{
    LOG("receive msg '%d'", cmd);
    switch (cmd) {
        case CMD_INIT:
            break;
        case CMD_START:
            break;
        case CMD_TICK:
            {
                int size = 0, fmt = 0, bitdepth = 0;
                //
                // A new frame is available: firstly, get the buffer address / size / format
                //
                unsigned char* pInframeBuffer = (unsigned char*)libvMI_get_frame_buffer(hFrame);
                libvMI_get_frame_headers(hFrame, MEDIA_PAYLOAD_SIZE, &size);
                libvMI_get_frame_headers(hFrame, MEDIA_FORMAT, &fmt);
                libvMI_get_frame_headers(hFrame, VIDEO_DEPTH, &bitdepth);

                LOG("receive frame on input[%d], fmt=%d[%s], size=%d bytes, bitdepth=%d", in, fmt, (fmt == MEDIAFORMAT::VIDEO ? "video" : "audio"), size, bitdepth);

                if (fmt == MEDIAFORMAT::VIDEO && bitdepth == 10) {

                    // Convert from 10bits to 8 bits, "in place". We can do that as the final 
                    tools::convert10bitsto8bits(pInframeBuffer, size, pInframeBuffer);
                    int newSize = size * 8 / 10;

                    // Set correct headers
                    bitdepth = 8;
                    libvMI_set_frame_headers(hFrame, MEDIA_PAYLOAD_SIZE, &newSize);
                    libvMI_set_frame_headers(hFrame, VIDEO_DEPTH, &bitdepth);

                    // Send the frame to all output
                    int nb_output = libvMI_get_output_count(g_vMIModule);
                    for (int i = 0; i < nb_output; i++) {

                        libvMI_pin_handle out = libvMI_get_output_handle(g_vMIModule, i);
                        //LOG("Send buffer to output [%d]", out);
                        libvMI_send(g_vMIModule, out, hFrame);
                    }
                }
                else if (fmt == MEDIAFORMAT::VIDEO && bitdepth == 8) {

                    // Convert from 8bits to 10bits, can't be done "in place" 
                    int newSize = size * 10 / 8;

                    int w, h, framenumber, timestamp;
                    libvMI_get_frame_headers(hFrame, VIDEO_WIDTH, &w);
                    libvMI_get_frame_headers(hFrame, VIDEO_HEIGHT, &h);
                    libvMI_get_frame_headers(hFrame, MEDIA_FRAME_NB, &framenumber);
                    libvMI_get_frame_headers(hFrame, MEDIA_TIMESTAMP, &timestamp);

                    struct vMIFrameInitStruct init;
                    init._media_format = MEDIAFORMAT::VIDEO;
                    init._media_size = 0;
                    init._video_width = w;
                    init._video_height = h;
                    init._video_depth = 10;
                    init._video_smpfmt = SAMPLINGFMT::YCbCr_4_2_2;
                    libvMI_frame_handle newH = libvmi_frame_create_ext(init);
                    // keep same frame number and media timestamp
                    libvMI_set_frame_headers(newH, MEDIA_FRAME_NB, &framenumber);
                    libvMI_set_frame_headers(newH, MEDIA_TIMESTAMP, &timestamp);

                    if (newH) {

                        LOG("new frame #%d", newH);
                        unsigned char* pOutframeBuffer = (unsigned char*)libvMI_get_frame_buffer(newH);
                        tools::convert8bitsto10bits(pInframeBuffer, size, pOutframeBuffer);

                        // Send the frame to all output
                        int nb_output = libvMI_get_output_count(g_vMIModule);
                        for (int i = 0; i < nb_output; i++) {

                            libvMI_pin_handle out = libvMI_get_output_handle(g_vMIModule, i);
                            int result = libvMI_send(g_vMIModule, out, newH);
                            LOG("%s to send frame #%d to output [%d]", (result == 0 ? "OK" : "KO"), newH, out);
                        }
                        libvmi_frame_release(newH);
                    }
                }

                /*
                * Always release the current frame
                */
                libvmi_frame_release(hFrame);
        }
            break;
        case CMD_STOP:
            break;
        case CMD_QUIT:
            {
                std::unique_lock<std::mutex> lock(g_mtx);
                g_var.notify_all();
                lock.unlock();
            }
            break;
        default:
            LOG("unknown cmd %d ", cmd);
    }
}

/*
 * Description: main
 * @method main
 * @param int argc
 * @param char* argv[]
 * @return int
 */
int main(int argc, char* argv[]) {
    char preconfig[MSG_MAX_LEN];
    bool use_preconfig = false;

    bool demoMode = false;

    // Check parameters
    if (argc >= 2) {
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
                STRNCPY(preconfig, argv[i + 1], MSG_MAX_LEN);
                preconfig[MSG_MAX_LEN - 1] = '\0';
                use_preconfig = true;
            } else if (strcmp(argv[i], "-v") == 0) {
                tools::displayVersion();
                return 0;
            } else if (strcmp(argv[i], "-h") == 0) {
                std::cout << "usage: " << argv[0] << " [-h] [-v] [-c <config>]\n";
                std::cout << "         -h   display this help\n";
                std::cout << "         -v   display module version\n";
                std::cout << "         -c <config>  module configuration string \n";
                return 0;
            }
        }
    }

    LOG("-->");

    // set signal handler    
    if (signal(SIGINT, signal_handler) == SIG_ERR)
        LOG_ERROR("can't catch SIGINT");
    if (signal(SIGTERM, signal_handler) == SIG_ERR)
        LOG_ERROR("can't catch SIGTERM");

    /*
    * Init the libvMI: provide a callback, and a pre-configuraion if any. If no pre-configuration here,
    * libvMI will wait for configuration provided by supervisor
    */
    std::unique_lock<std::mutex> lock(g_mtx);
    g_vMIModule = libvMI_create_module(&libvMI_callback, (use_preconfig ? preconfig : NULL));
    if (g_vMIModule == LIBVMI_INVALID_HANDLE) {
        LOG_ERROR("invalid Module id. Abort!");
        return 0;
    }
    LOG_INFO("init COMPLETED");

    /*
    * Start the module. The lib will notify a CMD_START via the callback when Start is completed.
    * From this point, the module will starts to receive media frames from inputs
    */
    libvMI_start_module(g_vMIModule);
    LOG_INFO("start COMPLETED");

    /*
    * wait for exit cmd
    */
    g_var.wait(lock);
    lock.unlock();
    //usleep(6000000);

    /*
    * Stop the module. The lib will notify a CMD_STOP via the callback when Stop is completed.
    * From this point, the module will no longer received media frames from inputs.
    * Note that module resources are not free... We can use start again to restart data flow processing.
    */
    libvMI_stop_module(g_vMIModule);
    LOG_INFO("stop COMPLETED");

    /*
    * Close the module and free all resources.
    * Note that from this point, module handle and all inputs/outputs handles will be invalidated.
    */
    libvMI_close(g_vMIModule);
    g_vMIModule = LIBVMI_INVALID_HANDLE;
    LOG_INFO("close COMPLETED");

    LOG("<--");    return 0;
}
