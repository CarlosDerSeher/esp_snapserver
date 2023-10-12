# esp_snapserver

This is a HIGHLY unstable [snapcast server](https://github.com/badaix/snapcast) implementation for ESP32.
It is in a very early stage and more of a proof of concept and possiblity study of myself. I am very
surprised and pleased by the result though, as this little thig called ESP32 is very much capable of doing all that's needed
to get audio playback through [esp_snapclient](https://github.com/CarlosDerSeher/snapclient) or even regular 
[snapclient](https://github.com/badaix/snapcast). 

It does loads of stuff currently:

* DLNA audio (LPCM) input (Android App AirMusic) 

  as sample rate, bits and channels are hardcoded please ensure your device sends 44100:16:2
* flac encoding PCM audio (44100:16:2)
* snapserver <--> snapclient communication / protocol handling
* stream to 1 connected esp_snapclient

What it can't do
* Snapcast control interface isn't implemented yet, so things like volume control and the like won't work.

As stated before it is in a very early stage of development and if not started in the right order it will most certainly crash:

* start esp_snapserver (wait until ready)
* connect dlna and start playback 
* start connecting snapclient(s)
* never disconnect anything from now on or it will crash :)

I tested with 1 esp_snapclient and 2 regular snapclients started on my ubuntu machine.

DLNA part is very buggy and needs a lot of work. I am very unhappy with ADF implementation and I would like
to use another one but couldn't find anything that wouldn't require a lot effort to port to ESP32. I'd appreciate
suggestions on this one.

Client disconnects also make the server unstable at the moment, I'll look into it.

## How to build and flash
Get ESP ADF:

```
git clone https://github.com/espressif/esp-adf.git esp-adf-v2.6
cd esp-adf-v2.6/
git checkout v2.6
git submodule update --init --recursive
```

Get ESP IDF:

```
git clone https://github.com/espressif/esp-idf.git esp-idf-v5.1.1
cd esp-idf-v5.1.1/
git checkout v5.1.1
git submodule update --init --recursive
```

Run from IDF root directory

```
git apply $ADF_PATH/idf_patches/idf_v5.1_freertos.patch
```

Now get this repository, build and flash

```
git clone --recurse-submodules https://github.com/CarlosDerSeher/esp_snapserver.git
cd esp_snapserver
idf.py menuconfig	# you'll probably want to adjust the port in "snapserver Configuration"
idf.py build flash monitor
```

At last build a [esp_snapclient](https://github.com/CarlosDerSeher/snapclient) or install [snapclient](https://github.com/badaix/snapcast)
and listen to some music.

