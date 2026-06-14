# Vulkan WSI Layer (Fork for termux-x11)
A Vulkan layer that intercepts window system integration (WSI) calls and
provides swapchain, surface, and presentation support for GPUs whose ICD
lacks native WSI -- such as the Mali vendor driver, which exposes no DRM
render node and therefore cannot use standard Mesa WSI paths.

This is a fork of [Arm's vulkan-wsi-layer](https://gitlab.freedesktop.org/mesa/vulkan-wsi-layer)
via [ginkage's fork](https://github.com/ginkage/vulkan-wsi-layer)
via [Sky1-Linux's fork](https://github.com/Sky1-Linux/vulkan-wsi-layer)
, which added X11 DRI3 presentation support. I removed DRM related stuffs
and added support for termux-x11.

## Current status
It works with qualcomm's glibc version of their proprietary vulkan adreno driver,
with quite impressive performance. It completely replaced qualcomm's broken
DRI3 x11 wsi(which is probably only designed for Xwayland anyway)

It also added some hacks around the qualcomm driver, mainly disabled UBWC because
it's causing serious problems on my test device (Adreno 740)

It will not work with turnip(because some dma-buf related problems), but you probably wont need it when you are using turnip.

This wsi layer works well with zink, however the performance is not great tho.
(Mostly because zink loves to create tons of pipelines, when creating and destroying
pipelines are actually quite expensive in qualcomm's driver)

## Requirements
 - A copy of compatible qualcomm proprietary driver
 - vulkan loader
 - [leegao/bcn_layer](https://github.com/leegao/bcn_layer) (Not sure if it's actually needed, but I'm not gonna trust qualcomm on handling the BCn textures)
 - mesa built with zink support if you want to use zink with it

## Compatible driver versions
I'm not sure if the EULA of qualcomm's drivers allows me to redistribute the binaries
(probably not) and if it even allow me to share the url of drivers, but I assume not.
So I'll only provided those versions that works with this wsi layer:
| Version/Release Date | Driver archive name | sha256sum | Notes |
| ------ | ------ | ------ | ------ |
| r00066 | qcom-adreno_1.808.0-2_arm64.deb | `1b68052ddb0bb962da19c8a038e59576b99386c2d67b0adc9ff244bc9b3309f3` | |
| 250119 | qcom-adreno_1.0_qcm6490.tar.gz | `0ffd748398cf05256fd2fbb3f6bf66c29bcfc9bc847566d84b7bd9e1941e78d2` | |
| 250203 | qcom-adreno_1.0_qcm6490.tar.gz | `6e78dfde7735369dbf9fd15610f9cad56c46c4f107f117b8bb5bf98db0f70cb1` | |
| 260215 | qcom-adreno_1.855.1_armv8-2a.tar.gz | `6d7e21764566105fdd07ee753225df7395a91237aedff2677cc3106b556b8e7e` | |
| 260304 | qcom-adreno_1.855.2_armv8a.tar.gz | `8e0b463519321613fa96f057c7cd7f67fd72b22dd0feb3129ffaab1400f17065` | |

All these driver versions work well with this layer, with very minor unnoticeable
lag. They are tested with no major glitches spotted.
Usually you would assume newer drivers provide better performance but 
qualcomm doesnt behave like this. For older devices older versions tend to work better.

There are also some other versions that will work with this layer and can run
vkmark and other normal vulkan programs, but will have some weird glitches with zink.
A simple way to tell if the driver works with zink is to run xeglgears with it,
if all the gears are shown normally, you found a working driver.(If it's not
documented here pls file an issue)
I've tested most qualcomm glibc drivers released from r00026-r00085(I only used version
number to document the driver version in this range) and 250119-260609(these are
file upload dates on their drivers) However there might be some drivers I havent tested,
if u found one that works then feel free to create an issue and report it here.

To run these drivers, you'll need [msm_drm_gbm](https://git.codelinaro.org/clo/le/display/libgbm/), with the branch `display.qclinux.1.0.r1-rel`

## Tested environment
Tested with mesa zink 25.2.5, 26.1.2, 26.2.0-dev, in Arch Linux(running in LXC)
on a device with Adreno 740.

## Potential problems
Because the workarounds I used and the unstable nature of qualcomm's driver, there might
be some stability issues around that I didnt notice. Feel free to report it in issues.

## Todo
 - Have some compile time cmake option to control whether to apply the workarounds for Adrno 740(so on devices where it's fine to use ubwc we can use it)
 - Remove useless hacks which are introduced while debugging but dont do anything other than slowing down the performance
 - More tests
 - Better Zink performance(and just overall performance improvement)
 - Deslop the code

## AI Usage
AI is used in this layer's development.

## License
[MIT](./LICENSE)

## Original Readme
[README.md.old](./README.md.old)

## Q&A
- Does this layer works with Mali devices?

No...? Never tested with a Mali device, it could work but with those Adreno hacks there
it's unlikely. However you can give it a try and figure out things while debugging.
(Now think about it I've never owned a Mali phone, like ever)
- Does it work with devices other than Adreno 740?

Yes I believe, those drivers I listed seem to support far more devices than they claim,
tho it's not tested.
If you tested it on your device and it works, then feel free to open an issue to report
this discovery so we can have better document about it.
- Does it work with proot or chroot?

Proot? I'm not so sure. If you have access to `/dev/dma_heap/system` then it should work.
Chroot should work since I'm testing it with LXC, which is kinda like a fancy chroot with
namespace.
- How's the performance compared with Turnip?

It depends on the driver version you are using and the turnip driver version you are comparing to. It also depends on the device you are testing it on.
In my case I usually see 1.5x-1.8x better performance compared with Turnip(from mesa
25.2.5), tho compared with Turnip 26.1.2 it's slower. However the performance of Turnip
in 26.1.2 doesnt seem to be reliable or stable so idk.
- Does it work with a normal X11 server running with DRM/KMS?

Emm... Maybe? I cannot get xserver to display anything on the screen with modesetting,
so I really dont know, but with lorie patches from termux-x11 in theory it should
work.
- Did u test it with real world workloads?

Yes, I tested with webgl tests inside chromium and minecraft. There arent any major 
glitches spotted(I mean, not anymore) so I believe it's enough to release it.
- Wait it caused a termux-x11 segfault

Yeah this is spotted in my tests, tho it rarely happens and there arent really any
clues about how this happened. My theory is something in the xserver part has gone wrong
but I'm not so sure. It really doesnt happen often tho and I havent yet experienced once
it crashed in front of my face(it's always when termux-x11 is running in background) so
it's not like that big of an issue for now.
- But why when we already have Turnip for Adreno devices?

You see qualcomm's driver actually does offer quite the performance some versions of Turnip
drivers doesnt offer. It's also better to have multiple choices so when Turnip broke
something you rely on u can have something to switch to.
Also it's cool.
- You used AI in this??? UNFORGIVABLE!!! YOU SHOULD GO TO HELL!

Erm... OK. ~~(I kinda like hot demon girls ngl)~~
Seriously tho this repo is only meant to be a poc, and I'm just someone who kinda
knows what's going on and how to do it but dont really have the time&knowledge to
figure everything out independently... If you fear it might be too unstable then dont
use it in any production environment.
I'm open to anyone who want to take over this project and maintain it with human efforts.
