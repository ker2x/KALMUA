# KALMUA: Keru's awesome Le Mans Ultimate App (ALPHA Version)

- This small cmdline app for LMU allow you to map FFB scaling to a steering wheel button/rotary encoder. (like in iRacing)
- It also allows you to save FFB scaling per car and load it when you select the car in LMU. (like in iRacing)

Optionally, it add audio cue when ABS or TC trigger. If the car have no ABS or TC then it won't do anything. It only use the native functionallity of LMU provided by Studio 397 Official SDK. If it's not in there, then it does not exist. Therefore: **IT'S NOT TELEMETRY BASED FFB ! It passthrough the FFB data from the game to the wheel and only add scaling it per car based on the config.** So there is no latency or stuttering like with telemetry based FFB.

Performance & reliability is the N°1 priority. Any feature degrading it will not be implemented. Period. No exception.

## WARNING & SAFETY: "Invert Force Feedback" setting in LMU

**Lower your max FFB before using this app for the first time, especially if you have a strong wheel.**

It's not really a bug, it's just because of the way LMU Telemetry works.

Here is the issue: 
- The telemetry data from LMU does not include the "Invert Force Feedback" data. (afaik)
- So the app receive the same FFB data from the game regardless of the "Invert Force Feedback" setting in LMU.
- As the app is just a passthrough with scaling, it will scale the FFB data and send it to the wheel.
- If your wheelbase requires you to invert the FFB in LMU, the app will not invert it by default and **you will get the FFB in the wrong direction.** (wild oscillations, injury, it happens even to the best of us)
- See "how to use" below to handle this. (my wheelbase require FFB Inversion)


## How to use

- Download the latest release (not available yet), or compile using the CMakeLists.txt
- There is no installer, unzip it somewhere and open a terminal in that folder.
- run "KALMUA.exe setup" to create the config file and set the FFB scaling button/encoder. (you can also edit the config file manually if you prefer)
- run "KALMUA.exe" to start the app. It will run in the background and monitor LMU for car changes and button presses.
- You can rerun "KALMUA.exe setup" to change the button/encoder or FFB scaling values at any time without losing your saved FFB scaling per car.
- Or you can edit KALMUA.ini manually at will. Settings in the config file are self explanatory. Just make sure to save it after editing and restart the app to apply changes.


### If you need to invert the FFB:

After running the setup, edit KALMU.ini and **set invert_ffb to 1**. Save. Restart the app.

## FAQ

### Is it vibe coded ?

No, I wrote my first program in early 80's. I have been programming in various languages since then.
But i'm not familliar with directX so Claude Opus 4.7 helped me (read throught the comments in the code for more details about that).

### Is it a hack ?

No, it use the LMU SharedMemoryInterface from Studio 397 official SDK. See: 
- https://lemansultimate.com/le-mans-ultimate-releases-v1-3-update-with-final-elms-content-performance-updates/
- (your LMU install path)/Support/SharedMemoryInterface/ (copied in this repo for version consistancy but i'll have to keep it up to date)

### The sound is horrible.

Yes, it's a very basic sound generated on the fly using the Windows Beep function. This will be fixed later.

### Can I use it with other games ?

Nope. It only works with LMU and will only ever work with LMU. For iRacing, use MAIRA. 
It's not working with RF2 either. There are plenty of app that use RF2 shared memory plugin. But since there is no an official plugin specific to LMU, i'm using it.

### Any GUI planned ?

I don't need a GUI for this app, it's meant to run in the background and be configured using the config file. But if there is enough demand, or if i need it for myself in the future, I might do it.

### Can you add this and that ?

Perhaps, open an issue with your suggestion and I'll see if it's something I can add without making the app too complicated or bloated. 
But keep in mind that this app is meant to be simple and focused on its core functionality, so I won't add features that are not directly related to FFB scaling or audio cues for ABS/TC.

### Can you make it open source ?

It's already open source. Apache 2.0 license. See LICENSE file.

### Can you make a telemetry based FFB ?

- I don't know how to write a telemetry based FFB app that will not suck.
- I like the LMU native FFB and I don't want to mess with it. I just want to scale it per car and add audio cues for ABS/TC.
- **Most importanly: the SDK is not designed for it (feel like it's even designed AGAINST it) so you will just end up with a buggy mess that will feel worse than the native FFB.**

### But "LMU FFB App" is doing it, it's just a little buggy.

- Yep, and the difference between a "almost good enough" telemetry based FFB and a "perfect" passthrough FFB is huge. 
- I don't want to settle for "almost good enough" this thing need to be able to handle a 24h race without crashing or stuttering.
