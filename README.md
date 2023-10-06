# esp_snapserver

This is a HIGHLY unstable [snapcast server](https://github.com/badaix/snapcast) implementation for ESP32.
It is in a very early stage and more of a proof of concept and possiblity study of myself. I am very
surprised by the result though as this little thig called ESP32 is very much capable of doing all that's needed
to get audio playback through [esp_snapclient](https://github.com/CarlosDerSeher/snapclient). It does loads of stuff currently:

* DLNA audio in input (Android App AirMusic)

  as sample rate, bits and channels are hardcoded please ensure your device sends 44100:16:2
* flac encoding PCM audio (44100:16:2)
* snapserver <--> snapclient communication / protocol handling
* stream to 1 connected esp_snapclient

As stated before it is in a very early stage of development and if not started in the right order it will definitly crash:

* start esp_snapserver (wait until ready)
* connect dlna and start playback 
* start esp_snapclient
* (never disconnect dlna from now on :)

I only tested with 1 esp_snapclient at the moment as I only have 2 boards at hand at the time of writing. 
Using Ubuntu client makes the server crash, a thing which needs debugging.

DLNA part is also very buggy and needs a lot of work. I am very unhappy with ADF implementation and I would like
to use another one but couldn't find anything that wouldn't require a lot effort to port to ESP32. I'd appreciate
suggestions on this one.

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
idf.py build flash monitor
```

At last build a [esp_snapclient](https://github.com/CarlosDerSeher/snapclient) and listen to some music.

