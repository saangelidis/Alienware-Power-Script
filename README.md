# Alienware-Power-Script
Low level code / script made to optimize Alienware M16 for long battery life unplugged and maximum performance plugged in.

Because without optimizations it only lasts 40 minutes and I have 3 hour classes with no outlets.

It comes in the form of two executable files.
One of the files is a "ghost game", that we use to trick the AWCC into setting the performance plan to Battery, as well as turn off all the keyboard lights. 

As of the moment, the two files should be able to do the following.

When laptop is **not** plugged in:
* Lock refresh rate to 60 Hz
* Minimize brightness
* Switch to the Alienware battery plan
* Disable RGB keys
* Enable energy saver

When laptop is plugged in:
* Maximize refresh rate (240 Hz)
* Maximize brightness
* Switch to whatever Alienware plan you use regularly
* Disable energy saver

# How to use:

## Option 1 - Build your own .exe

### Step 1. Make the two files executables, then place them in the same directory, and run the AlienwarePowerScript.exe

on mingw64
* g++ -O2 -municode -mwindows -o AlienwarePowerScript.exe AlienwarePowerScript.cpp -lpowrprof -luser32 -lgdi32 -lole32 -loleaut32 -lwbemuuid -lshell32
* g++ -O2 -municode -mwindows -o AlienwareBatteryGame.exe AlienwareBatteryGame.cpp      

### Step 2. If it's your first time running it, run as **administrator** so that the powercfg commands can execute, and don't forget to configure your ghost game!

* First, add the game from AWCC
* then configure it to use the Battery plan
* then turn off the key lights as well.

Keep AWCC open so that it can detect the ghost game!

## Option 2 - Run the existing .exe

### Step 1. Place the two .exe files in the same directory and run 'AlienwarePowerScript.exe'

Follow Option 1 Step 2 from there.
