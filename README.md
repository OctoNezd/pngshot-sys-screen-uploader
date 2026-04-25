# pngshot-sys-screen-uploader

pngshot-ssu is a modification of pngshot which allows to make screenshots in png, and modification is that screenshots are sent to telegram or discord using screenuploader.bakatrouble.me service.

## AI Disclaimer

99% of changes are written by AI, particularly Claude Opus 4.7 using Cline plugin. The whole thing costed me like 30 bucks, but mostly cause I thought of idiotic implementation for first time. If you are not comfortable with using it cause of that - thats okay. 

If you think you can make a better plugin yourself - please do! I know whatever claude wrote isnt perfect - for example adrenaline screenshots are often corrupted - but it seems to be inherited from pngshot itself. And screenshots arent particularly fast too - but it seems to be case with pngshot as well. 

## Notice for my fellow Russian RKN "enjoyers"

Apparently uploading photos is now illegal, at least with my ISP. Without proxy uploads freeze:

```bash
curl -v "http://screenuploader.bakatrouble.me/upload/<TOKEN>/?filename=2026042517420500-00000000000000000000000000000000.png" --upload-file photo_2026-04-25_17-44-19.jpg -X POST                            
* Host screenuploader.bakatrouble.me:80 was resolved.
* IPv6: (none)
* IPv4: 188.166.152.102
*   Trying 188.166.152.102:80...
* Established connection to screenuploader.bakatrouble.me (188.166.152.102 port 80) from 192.168.88.3 port 58630 
* using HTTP/1.x
> POST /upload/<TOKEN>/?filename=2026042517420500-00000000000000000000000000000000.png HTTP/1.1
> Host: screenuploader.bakatrouble.me
> User-Agent: curl/8.19.0
> Accept: */*
> Content-Length: 83543
> 
* upload completely sent off: 83543 bytes
```

So the only option for now is to run proxy OR you can use server somewhere outside of this place redirecting to bakatrouble's. Example caddyfile config for that:

```Caddyfile
:80 {
reverse_proxy https://screenuploader.bakatrouble.me:443
}
```

## Features

* Takes screenshots in png format
* No watermark
* Take screenshots in any app

## Installation

Download from the [Releases section](https://github.com/octonezd/pngshot-ssu/releases) or [CI](https://nightly.link/OctoNezd/pngshot-sys-screen-uploader/workflows/build.yaml/master).

Copy `pngshot-ssu.suprx` to `ur0:tai` and add `ur0:tai/pngshot-ssu.suprx` below `*main` in `ur0:tai/config.txt`. 

IMPORTANT: Make sure to disable pngshot plugin. I didnt test them together, but I suppose they will conflict.

If you also want the Photos app email hook to upload photos to ssu, add the plugin under `*NPXS10004` as well:

```
*NPXS10004
ur0:tai/pngshot-ssu.suprx
```

Create ux0:data/pngshot-ssu directory. Write config.txt, based on contents of config.sample.txt in this repo.

## Usage

Press PS button + Start to take a screenshot. You can access screenshots with the Photos app, or from `ux0:picture/SCREENSHOT`.

## Additional notes

To compile this plugin from source, you need a custom build of libpng (the one in vdpm will not work). Check out `libpng` directory for a working `VITABUILD`.

## Debugging

I used https://github.com/TeamFAPS/PSVita-RE-tools/blob/master/README.md#princesslog-usage to look at logs. Seems to work pretty good.