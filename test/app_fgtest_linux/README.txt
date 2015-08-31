Frame grabber test
==================================

app_fgtest_linux is an applications used to test Xylon logiWIN IP core frame grabber driver.

app_fgtest main functions are:
 - video in
 - video in scaling/cropping 
 - video in positioning 

Building 
-------------

To build this application, you need to do the following:

1. Make sure `CC` variable in Makefile corresponds
   to the installed cross-compiler.


2. Make sure cross-compiler set in `CC` variable is added accessible from the
   command line where building of the application will be performed. If using
   Vivado 2014.4 SDK execute following command.

    bash> source /opt/Xilinx/SDK/2014.4/settings32.sh


3. To build the application execute:

    bash> make

   Copy resulting executable to the SD card.


4. Run binary on the target

    bash> /mnt/app_fgtest_linux -conoff -o /dev/fb1 -s 1280x720             # Capturing video in full screen
    bash> /mnt/app_fgtest_linux -conoff -o /dev/fb1 -s 1280x720 -pos        # Run the video positioning demo  
    bash> /mnt/app_fgtest_linux -conoff -o /dev/fb1 -s 1280x720 -sc         # Run the video scaling/cropping demo
