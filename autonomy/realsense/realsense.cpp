/**
  Compute 3D points from RealSense data.
  
  This needs librealsense2, from https://github.com/IntelRealSense/librealsense
  
  Dr. Orion Lawlor, lawlor@alaska.edu (public domain)
  Modified from tiny example by BjarneG at https://communities.intel.com/thread/121826
*/
#include <librealsense2/rs.hpp>  
#include <opencv2/opencv.hpp>  
#include "vision/grid.hpp"
#include "vision/grid.cpp"
#include "vision/terrain_map.cpp"
#include "../firmware/field_geometry.h"

#include "aurora/beacon_commands.h"
#include "find_obstacles.h"


#include "aruco_localize.cpp"
  
using namespace std;  
using namespace cv;  

bool show_GUI=true; // show debug windows onscreen

#define DO_GCODE 0 /* command 3D printer via serial gcode */
#if DO_GCODE
#include "printer_gcode.h"
#endif
#include "serial.cpp" /* for talking to stepper driver */

SerialPort stepper_serial;
bool pan_stepper=true; // automatically pan stepper motor around

int sys_error=0;

/// Rotate coordinates using right hand rule
class coord_rotator {
public:
  const real_t angle; // rotation angle in radians
  const real_t c,s; // cosine and sine of rotation angle
  coord_rotator(real_t angle_degs=0.0) 
    :angle(angle_degs*M_PI/180.0), c(cos(angle)), s(sin(angle)) 
  { }
  
  inline void rotate(real_t &x,real_t &y) const {
    real_t new_x = x*c - y*s;
    real_t new_y = x*s + y*c;
    x=new_x; y=new_y;
  }
};

/// Transforms 3D points from depth camera coords to world coords,
///  by rotating and translating
class camera_transform {
public:  
  vec3 camera; // world-coordinates camera origin position (cm)
  coord_rotator camera_tilt; // tilt down
  coord_rotator Z_rotation; // camera panning
  
  camera_transform(real_t camera_Z_angle=0.0)
    :camera(field_x_beacon,field_y_beacon,70.0),  // camera position
     camera_tilt(-20), // X axis rotation (camera mounting tilt)
     Z_rotation(camera_Z_angle) // Z axis rotation
  {
  }
  
  // Project this camera-relative 3D point into world coordinates
  vec3 world_from_camera(vec3 point) const {
    real_t x=point.z, y=-point.x, z=-point.y;
    camera_tilt.rotate(y,z); // tilt up, so camera is level
    Z_rotation.rotate(x,y); // rotate, to align with field
    x+=camera.x;
    y+=camera.y;
    z+=camera.z; 
    return vec3(x,y,z);
  }
};

/* Transforms raw realsense 2D + depth pixels into 3D:
  Camera X is along sensor's long axis, facing right from sensor point of view
  Camera Y is facing down
  Camera Z is positive into the frame
*/
class realsense_projector {
public:
  // Camera calibration
  rs2_intrinsics intrinsics;
  
  // Cached per-pixel direction vectors: scale by the depth to get to 3D
  std::vector<float> xdir;
  std::vector<float> ydir;
  
  realsense_projector(const rs2::depth_frame &frame)
    :xdir(frame.get_width()*frame.get_height()),
     ydir(frame.get_width()*frame.get_height())
  {
    auto stream_profile = frame.get_profile();
    auto video = stream_profile.as<rs2::video_stream_profile>();
    intrinsics = video.get_intrinsics();
    
    // Precompute per-pixel direction vectors (with distortion)
    for (int h = 0; h < intrinsics.height; ++h)
    for (int w = 0; w < intrinsics.width; ++w)
    {
      const float pixel[] = { (float)w, (float)h };

      float x = (pixel[0] - intrinsics.ppx) / intrinsics.fx;
      float y = (pixel[1] - intrinsics.ppy) / intrinsics.fy;

      if (intrinsics.model == RS2_DISTORTION_INVERSE_BROWN_CONRADY)
      {
          float r2 = x * x + y * y;
          float f = 1 + intrinsics.coeffs[0] * r2 + intrinsics.coeffs[1] * r2*r2 + intrinsics.coeffs[4] * r2*r2*r2;
          float ux = x * f + 2 * intrinsics.coeffs[2] * x*y + intrinsics.coeffs[3] * (r2 + 2 * x*x);
          float uy = y * f + 2 * intrinsics.coeffs[3] * x*y + intrinsics.coeffs[2] * (r2 + 2 * y*y);
          x = ux;
          y = uy;
      }

      xdir[h*intrinsics.width + w] = x;
      ydir[h*intrinsics.width + w] = y;
    }
  }
  
  // Project this depth at this pixel into 3D camera coordinates
  vec3 lookup(float depth,int x,int y) 
  {
    int i=y*intrinsics.width + x;
    return vec3(xdir[i]*depth, ydir[i]*depth, depth);
  }
};



/// Keeps track of platform position
class stepper_controller
{
  /* Angle (degrees) that centerline of camera is facing */
  float camera_Z_angle;
  int last_steps;
#define stepper_angle_to_step (400.0 / 360.0)  /* scale degrees to step count */
#define stepper_angle_zero -73 /* degrees for stepper zero */
  
  /* Poll on the serial port.  Return true if stuff was read. */
  bool read_serial() {
    bool read_stuff=false;
    while (stepper_serial.available()) {
      read_stuff=true;
      unsigned char pos255=stepper_serial.read();
      camera_Z_angle=(pos255 / stepper_angle_to_step)+stepper_angle_zero;
      
      static unsigned char last=255;
      if (pos255!=last) {
        printf("Stepper reports %.0f degrees / %d steps\n",camera_Z_angle,pos255);
        last=pos255;
      }
    }
    return read_stuff;
  }

  void seek_steps(int step) {
    if (step<0) step=0;
    if (step>250) step=250;
    stepper_serial.write((unsigned char)step);
  }

public:
  // Observed angle - true angle correction factor
  float angle_correction;

  stepper_controller() 
    :angle_correction(0.0)
  {
    last_steps=0;
    if (pan_stepper)
    {
    // Connect to Arduino Nano, running nano_stepper firmware.
      stepper_serial.Set_baud(57600);
      if (stepper_serial.Open("/dev/ttyUSB0")!=0) {
        pan_stepper=false;
      } else {
        // Wait for bootloader to finish
        printf("*** Successfully connected to stepper\n");
        sleep(2); // seconds to wait for bootloader
        read_serial();
      }
    }
  }
  bool serial_poll(void) {
    if (pan_stepper) {
      return read_serial();
    }
    else return true;
  }
  
  void setup_seek(void) {
    if (pan_stepper) {
      camera_Z_angle=stepper_angle_zero;
      stepper_serial.write(0xff);
    }
  }
  void absolute_seek(float degrees) {
    if (pan_stepper) {
      int step=(degrees-angle_correction-stepper_angle_zero)*stepper_angle_to_step;
      printf("*** Seeking stepper to %.0f degrees / %d steps\n",degrees,step);
      seek_steps(step);
    } else { camera_Z_angle=degrees-angle_correction; }
  }
  // Return the current stepper angle, in degrees 
  float get_angle_deg(void) const {
    return camera_Z_angle+angle_correction;
  }
};


#include "aurora/pose.h"
#include "aurora/pose_network.h"
#include "aruco_marker_IDs.h" /* location and size of markers */

/** 
Watches for OpenCV markers, and prints them as it sees them.
*/
class marker_watcher_print {
  const camera_transform &camera_TF;
public:
  robot_markers_all markers;
  float angle_correction;

  marker_watcher_print(const camera_transform &camera_TF_) 
    :camera_TF(camera_TF_), angle_correction(0.0f)
  {
  }

  void found_marker(cv::Mat &m,const aruco::Marker &marker,int ID)
  {
    const marker_info_t &info=get_marker_info(ID);
    
    double scale=info.true_size;
    if (scale<0.0) { // invalid marker
       printf("Unknown marker ID %d in view\n",ID);
       return;
    }
    vec3 v; // camera-coords location
    v.x=+m.at<float>(0,3)*scale;
    v.y=+m.at<float>(1,3)*scale;
    v.z=+m.at<float>(2,3)*scale;
    
    // World coordinates of center point of observed marker
    vec3 w=camera_TF.world_from_camera(v);

    if (info.side<0) { // marker is fixed to trough, angular reference only
      w -= camera_TF.camera; // marker positions are camera-relative
      float deg=(atan2(w.y,w.x))*(180.0/M_PI); // observed position
      float ref=info.shift.z; // theoretical position
      printf("Angle shift %.1f (ref %.0f, observed %.1f, (%.2f,%.2f,%.2f)\n",
          deg-ref, ref, deg, w.x,w.y,w.z);
      angle_correction=deg-ref;
      return;
    }
    
    
    // bin.angle=180.0/M_PI*atan2(m.at<float>(2,0),m.at<float>(0,0));
    vec3 axes[3];
    for (int axis=0;axis<3;axis++) {
      vec3 a;
      a.x=+m.at<float>(0,axis)*scale;
      a.y=+m.at<float>(1,axis)*scale;
      a.z=+m.at<float>(2,axis)*scale;
      a=a*100.0; // meters to centimeters
      
      axes[axis]=camera_TF.world_from_camera(v+a)-w;
    }
    // printf("X axis: %.2f %.2f %.2f\n",axes[0].x,axes[0].y,axes[0].z);
    float radian2degree=180.0/M_PI;
    float yaw=radian2degree * atan2(axes[0].y,axes[0].x);
    
    // FIXME: these should be robot-relative, not world-relative rotations
    // pitch seems particularly unpredictable
    float roll=radian2degree * atan2(axes[0].z,axes[0].x);
    float pitch=radian2degree * atan2(axes[2].y,-axes[2].z);
    
    // Print grep-friendly output
    printf("Marker %d: ", info.id);
    if (false) {
       printf("Camera %.1f  World %.1f %.1f %.1f cm, yaw %.1f deg, roll %.1f deg, pitch %.1f deg\n",
           camera_TF.camera.y,  w.x,w.y,w.z, yaw,roll,pitch
          );
    }
  
    markers.add(info.id, w,axes[0],axes[1], info.shift, info.weight, info.side);
    markers.markers[info.id].print();

    fflush(stdout);
  }
};


int main(int argc,const char *argv[])  
{  
    rs2::pipeline pipe;  
    rs2::config cfg;  
  
    bool bigmode=true; // high res 720p input
    bool do_depth=false; // auto-read depth frames, parse into grid
    bool do_color=true; // read color frames, look for vision markers
    int fps=6; // framerate (USB 2.0 compatible by default)
    
    for (int argi=1;argi<argc;argi++) {
      std::string arg=argv[argi];
      if (arg=="--nogui") show_GUI=false;
      else if (arg=="--depth") do_depth=true;
      else if (arg=="--nodepth") do_depth=false;
      else if (arg=="--nocolor") do_color=false;
      else if (arg=="--coarse") bigmode=false; // lowres mode
      else if (arg=="--nostep") pan_stepper=false; // pan around
      else if (arg=="--fast") fps=30; // USB-3 only
      else {
        std::cerr<<"Unknown argument '"<<arg<<"'.  Exiting.\n";
        return 1;
      }
    }
    
#if DO_GCODE
    printf("Connecting to 3D printer over serial port...\n");
    printer_gcode gcode;
    printf("Connected.  Initializing...\n");
    gcode.send(
      "G21\n" // set units to mm
      "G90\n" // absolute moves
      //"G28 Y0\n" // home Y
      "G0 Y0 F5000\n" // move to Y==0
      "M114\n" // report position
      );
    gcode.wait("ok");
    gcode.poll();
    printf("Connecting realsense\n");
#endif
    
    stepper_controller stepper;
    //stepper.setup_seek();  // <- assumes startup in most clockwise angle
    stepper.absolute_seek(0); // start in about the right direction

    int depth_w=1280, depth_h=720; // high res mode: definitely more detail visible
    int color_w=1280, color_h=720; 
    if (!bigmode) { // low res
      if (fps<10) fps=15;
      depth_w=480; depth_h=270;
      color_w=640; color_h=480; //color_w=424; color_h=240;
    }

    cfg.enable_stream(RS2_STREAM_DEPTH, depth_w,depth_h, RS2_FORMAT_Z16, fps);  
    cfg.enable_stream(RS2_STREAM_COLOR, color_w,color_h, RS2_FORMAT_BGR8, fps);  
  
    rs2::pipeline_profile selection = pipe.start(cfg);  

    auto sensor = selection.get_device().first<rs2::depth_sensor>();
    float scale =  sensor.get_depth_scale();
    printf("Depth scale: %.3f\n",scale);
    double depth2cm = scale * 100.0; 
    double depth2screen=255.0*scale/4.5;
    
    pose_publisher pose_pub;
    
    int framecount=0;
    int writecount=0;
    int nextwrite=1;

    int obstacle_scan=0; // frames remaining for obstacle scan
    int obstacle_scan_target=-999; // angle at which we want to do the scan
    
    aruco_localizer aruco_loc;

    obstacle_grid obstacles;
    
    aurora_beacon_command_server command_server;
    
    rs2::frameset frames;  
    while (true)  
    {  
        // Check for network data
        try {
        aurora_beacon_command cmd;
        if (command_server.request(cmd)) {
          cmd.letter=toupper(cmd.letter); // uppercase request char
          if (cmd.letter=='P') { // point request
            stepper.absolute_seek(cmd.angle);
            command_server.response(); 
          }
          else if (cmd.letter=='O') { // turn-off request
            sys_error=system("sudo shutdown -h now");
            command_server.response(); 
          }
          else if (cmd.letter=='H') { // hard-home the stepper
            stepper.setup_seek();
            while (!stepper.serial_poll()) { /* wait for stepper to home */ }
            stepper.angle_correction=0.0;
            stepper.absolute_seek(cmd.angle);
            command_server.response(); 
          }
          else if (cmd.letter=='T') { // scan for obstacles
            stepper.absolute_seek(cmd.angle);
            obstacle_scan_target=cmd.angle;
            obstacles.clear();
            obstacle_scan=18;  // frames to scan 
          }
          else { // unknown command
            printf("Ignoring unknown command request '%c'\n", cmd.letter);
            command_server.response(); 
          }
        }
        }
        catch (...) {
          printf("Ignoring network exception\n");
        }

        // Figure out coordinate system for this capture
        stepper.serial_poll();
        camera_transform camera_TF(stepper.get_angle_deg());

#if DO_GCODE
        camera_TF.camera=vec3(); // zero out camera position
        
        gcode.poll();
        int startframe=100;
        
        if (framecount>startframe) {
          float maxmove=900;
          int frames_per_mm=6;
          float moveto=(framecount-startframe)*(1.0/frames_per_mm);
          printf("Gcode move to y=%f\n",moveto);
          if (moveto>maxmove) { fflush(stdout); break; }
          gcode.send("G0 Y"); gcode.send(moveto); gcode.send("\n");
          gcode.send("M114\n"); // report position
          
          camera_TF.camera.y=moveto*0.1; // <- mm to cm here
        }
#endif


        // Wait for a coherent pair of frames: depth and color
        frames = pipe.wait_for_frames();  
        rs2::video_frame color_frame = frames.get_color_frame();  
        rs2::depth_frame depth_frame = frames.get_depth_frame();  
        if ((depth_w != depth_frame.get_width()) ||
            (depth_h != depth_frame.get_height()) || 
            (color_w != color_frame.get_width()) ||
            (color_h != color_frame.get_height()))
        {
          std::cerr<<"Realsense capture size mismatch!\n";
          exit(1);
        }
        
        framecount++;
  
        void *color_data = (void*)color_frame.get_data();  
          
        // Make OpenCV version of raw pixels (no copy, so this is cheap)
        Mat color_image(Size(color_w, color_h), CV_8UC3, color_data, Mat::AUTO_STEP);  
        if (do_color) 
        {
          //imshow("RGB", color_image);

          marker_watcher_print p(camera_TF);
          
#if DO_GCODE
          static std::string last_name="";
          char name[100]; 
          snprintf(name,100,"gcode_vidcap/frame_%03dcm.jpg",(int)(camera_TF.camera.y));
          if (name!=last_name) {
             imwrite(name,color_image);
             last_name=name;
          }
          
          if (camera_TF.camera.y!=0.0)
#endif
          aruco_loc.find_markers(color_image,p);
          if (p.angle_correction!=0) {
             stepper.angle_correction-=p.angle_correction;
          }
          p.markers.pose.print();
          p.markers.beacon=stepper.get_angle_deg();
          pose_pub.publish(p.markers);
          
          if (show_GUI) 
            imshow("Color Image",color_image);
        }
        
        bool do_scan=(obstacle_scan>0);
        if (do_scan && pan_stepper && fabs(obstacle_scan_target-stepper.get_angle_deg())>4.0)
        {
          do_scan=false; // wait until stepper reaches target (FIXME: hangs if coords corrected during seek?)
        }
        
        if (do_depth || obstacle_scan>0) 
        {
          typedef unsigned short depth_t;
          depth_t *depth_data = (depth_t*)depth_frame.get_data();
          
          // Display raw data onscreen
          //Mat depth_raw(Size(depth_w, depth_h), CV_16U, depth_data, Mat::AUTO_STEP);    
          //imshow("Depth", depth_raw);
          
          // Set up *static* 3D so we don't need to recompute xdir and ydir every frame.
          static realsense_projector depth_to_3D(depth_frame);
          
          //Mat debug_image(Size(depth_w, depth_h), CV_8UC3, cv::Scalar(0));
          
          // if (obstacle_scan>=16) obstacles.clear(); // clear the grid
          
          const int realsense_left_start=50; // invalid data left of here
          for (int y = 0; y < depth_h; y++)
          for (int x = realsense_left_start; x < depth_w; x++)
          {
            int i=y*depth_w + x;
            float depth=depth_data[i]*depth2cm; // depth, in cm
            
            int depth_color=depth*(255.0/400.0);
            cv::Vec3b debug_color;
            if (depth_color<=255) debug_color=cv::Vec3b(depth_color,depth_color,0);
            
            if (depth>0) {
              vec3 cam = depth_to_3D.lookup(depth,x,y);
              vec3 world = camera_TF.world_from_camera(cam);
              
              if (world.z<150.0 && world.z>-50.0)
              {
                obstacles.add(world);
              }
              
            }
            //debug_image.at<cv::Vec3b>(y,x)=debug_color;
          }   
          //imshow("Depth image",debug_image);
          
          if (show_GUI) {
            cv::Mat world_depth=obstacles.get_debug_2D(6);
            imshow("2D World",world_depth);    
          }
          if (obstacle_scan>0) { 
            obstacle_scan--;
            if (obstacle_scan==0) {
              // Done with scan--report results to backend
              imwrite("raw_color.png",color_image);
              std::vector<aurora_detected_obstacle> obstacle_list;
              find_obstacles(obstacles,obstacle_list);
              command_server.response(&obstacle_list[0],
                sizeof(obstacle_list[0])*obstacle_list.size()); 
            }
          }
        }    
        
        int k = waitKey(10);  
  if ((framecount>=30) || k == 'i') // image dump 
  {
    framecount=0;
    char filename[500];
    if (do_depth) {
      sprintf(filename,"vidcaps/world_depth_%03d",(int)(0.5+stepper.get_angle_deg()));
      obstacles.write(filename);
      printf("Stored image to file %s\n",filename);
      obstacles.clear();
    }
    if (do_color) {
      imwrite("vidcaps/latest.jpg",color_image);
      sprintf(filename,"cp vidcaps/latest.jpg vidcaps/view_%04d_%03ddeg.jpg",writecount,(int)(0.5+stepper.get_angle_deg()));
      sys_error=system(filename);
    }
    
    if (0 && pan_stepper) { // demo stepper seeking, in pattern
      const int n_angles=5;
      const static float angles[n_angles]={-45,0,+45,+60,0};
      stepper.absolute_seek(angles[writecount%n_angles]);
    }

    writecount++;
  }
        if (k == 27 || k=='q')  
            break;  
    }  
  
  
    return 0;  
}  
