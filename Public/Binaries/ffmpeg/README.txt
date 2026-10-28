FFmpeg 64-bit static Windows build from www.gyan.dev

Version: 2026-09-21-git-a9cbcc2bbb-full_build-www.gyan.dev

License: GPL v3

Source Code: https://github.com/FFmpeg/FFmpeg/commit/a9cbcc2bbb

External Assets
frei0r plugins:   https://www.gyan.dev/ffmpeg/builds/ffmpeg-frei0r-plugins
lensfun database: https://www.gyan.dev/ffmpeg/builds/ffmpeg-lensfun-db
whisper models:   https://huggingface.co/ggerganov/whisper.cpp/tree/main

git-full build configuration: 

ARCH                      x86 (generic)
big-endian                no
runtime cpu detection     yes
standalone assembly       yes
x86 assembler             nasm
MMX enabled               yes
MMXEXT enabled            yes
SSE enabled               yes
SSSE3 enabled             yes
AESNI enabled             yes
CLMUL enabled             yes
AVX enabled               yes
AVX2 enabled              yes
AVX-512 enabled           yes
AVX-512ICL enabled        yes
XOP enabled               yes
FMA3 enabled              yes
FMA4 enabled              yes
i686 features enabled     yes
CMOV is fast              yes
EBX available             yes
6 registers available     yes
7 registers available     yes
debug symbols             yes
strip symbols             yes
optimize for size         no
optimizations             yes
static                    yes
shared                    no
network support           yes
threading support         pthreads
safe bitstream reader     yes
texi2html enabled         no
perl enabled              yes
pod2man enabled           yes
makeinfo enabled          yes
makeinfo supports HTML    yes
experimental features     yes
xmllint enabled           yes

External libraries:
avisynth                libgsm                  libsvtjpegxs
bzlib                   libharfbuzz             libtheora
cairo                   libilbc                 libtwolame
chromaprint             libjxl                  libuavs3d
frei0r                  liblc3                  libvidstab
gmp                     liblensfun              libvmaf
gnutls                  libmodplug              libvo_amrwbenc
iconv                   libmp3lame              libvorbis
ladspa                  libmysofa               libvpx
lcms2                   liboapv                 libvvenc
libaom                  libopencore_amrnb       libwebp
libaribb24              libopencore_amrwb       libx264
libaribcaption          libopenjpeg             libx265
libass                  libopenmpt              libxavs2
libbluray               libopus                 libxevd
libbs2b                 libplacebo              libxeve
libcaca                 libqrencode             libxml2
libcdio                 libquirc                libxvid
libcodec2               librav1e                libzimg
libdav1d                librist                 libzmq
libdavs2                librubberband           libzvbi
libdvdnav               libshine                lzma
libdvdread              libsnappy               mediafoundation
libflite                libsoxr                 openal
libfontconfig           libspeex                sdl2
libfreetype             libsrt                  whisper
libfribidi              libssh                  zlib
libgme                  libsvtav1

External libraries providing hardware acceleration:
amf                     d3d12va                 nvdec
cuda                    dxva2                   nvenc
cuda_llvm               ffnvcodec               opencl
cuvid                   libmfx                  vaapi
d3d11va                 libvpl                  vulkan

Libraries:
avcodec                 avformat                swscale
avdevice                avutil
avfilter                swresample

Programs:
ffmpeg                  ffplay                  ffprobe

Enabled decoders:
aac                     flv                     pcm_u16le
aac_fixed               fmvc                    pcm_u24be
aac_latm                fourxm                  pcm_u24le
aasc                    fraps                   pcm_u32be
ac3                     frwu                    pcm_u32le
ac3_fixed               ftr                     pcm_u8
acelp_kelvin            g2m                     pcm_vidc
adpcm_4xm               g723_1                  pcx
adpcm_adx               g728                    pdv
adpcm_afc               g729                    pfm
adpcm_agm               gdv                     pgm
adpcm_aica              gem                     pgmyuv
adpcm_argo              gif                     pgssub
adpcm_circus            gremlin_dpcm            pgx
adpcm_ct                gsm                     phm
adpcm_dtk               gsm_ms                  photocd
adpcm_ea                h261                    pictor
adpcm_ea_maxis_xa       h263                    pixlet
adpcm_ea_r1             h263i                   pjs
adpcm_ea_r2             h263p                   png
adpcm_ea_r3             h264                    ppm
adpcm_ea_xas            h264_amf                prores
adpcm_g722              h264_cuvid              prores_raw
adpcm_g726              h264_qsv                prosumer
adpcm_g726le            hap                     psd
adpcm_ima_acorn         hca                     ptx
adpcm_ima_alp           hcom                    qcelp
adpcm_ima_amv           hdr                     qdm2
adpcm_ima_apc           hevc                    qdmc
adpcm_ima_apm           hevc_amf                qdraw
adpcm_ima_cunning       hevc_cuvid              qoa
adpcm_ima_dat4          hevc_qsv                qoi
adpcm_ima_dk3           hnm4_video              qpeg
adpcm_ima_dk4           hq_hqa                  qtrle
adpcm_ima_ea_eacs       hqx                     r10k
adpcm_ima_ea_sead       huffyuv                 r210
adpcm_ima_escape        hymt                    ra_144
adpcm_ima_hvqm2         iac                     ra_288
adpcm_ima_hvqm4         idcin                   ralf
adpcm_ima_iss           idf                     rasc
adpcm_ima_magix         iff_ilbm                rawvideo
adpcm_ima_moflex        ilbc                    realtext
adpcm_ima_mtf           imc                     rka
adpcm_ima_oki           imm4                    rl2
adpcm_ima_pda           imm5                    roq
adpcm_ima_qt            indeo2                  roq_dpcm
adpcm_ima_rad           indeo3                  rpza
adpcm_ima_smjpeg        indeo4                  rscc
adpcm_ima_ssi           indeo5                  rtv1
adpcm_ima_wav           interplay_acm           rv10
adpcm_ima_ws            interplay_dpcm          rv20
adpcm_ima_xbox          interplay_video         rv30
adpcm_ms                ipu                     rv40
adpcm_mtaf              jacosub                 rv60
adpcm_n64               jpeg2000                s302m
adpcm_psx               jpegls                  sami
adpcm_psxc              jv                      sanm
adpcm_sanyo             kgv1                    sbc
adpcm_sbpro_2           kmvc                    scpr
adpcm_sbpro_3           lagarith                screenpresso
adpcm_sbpro_4           lead                    sdx2_dpcm
adpcm_swf               libaom_av1              sga
adpcm_thp               libaribb24              sgi
adpcm_thp_le            libaribcaption          sgirle
adpcm_vima              libcodec2               sheervideo
adpcm_xa                libdav1d                shorten
adpcm_xmd               libdavs2                simbiosis_imx
adpcm_yamaha            libgsm                  sipr
adpcm_zork              libgsm_ms               siren
agm                     libilbc                 smackaud
ahx                     libjxl                  smacker
aic                     libjxl_anim             smc
alac                    liblc3                  smvjpeg
alias_pix               libopencore_amrnb       snow
als                     libopencore_amrwb       sol_dpcm
amrnb                   libopus                 sp5x
amrwb                   libspeex                speedhq
amv                     libsvtjpegxs            speex
anm                     libuavs3d               srgc
ansi                    libvorbis               srt
anull                   libvpx_vp8              ssa
apac                    libvpx_vp9              stl
ape                     libxevd                 subrip
apng                    libzvbi_teletext        subviewer
aptx                    loco                    subviewer1
aptx_hd                 lscr                    sunrast
apv                     m101                    svq1
arbc                    mace3                   svq3
argo                    mace6                   tak
ass                     magicyuv                targa
asv1                    mdec                    targa_y216
asv2                    media100                tdsc
atrac1                  metasound               text
atrac3                  microdvd                theora
atrac3al                mimic                   thp
atrac3p                 misc4                   tiertexseqvideo
atrac3pal               mjpeg                   tiff
atrac9                  mjpeg_cuvid             tmv
aura                    mjpeg_qsv               truehd
aura2                   mjpegb                  truemotion1
av1                     mlp                     truemotion2
av1_amf                 mmvideo                 truemotion2rt
av1_cuvid               mobiclip                truespeech
av1_qsv                 motionpixels            tscc
avrn                    movtext                 tscc2
avrp                    mp1                     tta
avs                     mp1float                twinvq
avui                    mp2                     txd
bethsoftvid             mp2float                ulti
bfi                     mp3                     utvideo
bink                    mp3adu                  v210
binkaudio_dct           mp3adufloat             v210x
binkaudio_rdft          mp3float                vb
bintext                 mp3on4                  vble
bitpacked               mp3on4float             vbn
bmp                     mpc7                    vc1
bmv_audio               mpc8                    vc1_cuvid
bmv_video               mpeg1_cuvid             vc1_qsv
bonk                    mpeg1video              vc1image
brender_pix             mpeg2_cuvid             vcr1
c93                     mpeg2_qsv               vmdaudio
cavs                    mpeg2video              vmdvideo
cbd2_dpcm               mpeg4                   vmix
ccaption                mpeg4_cuvid             vmnc
cdgraphics              mpegvideo               vnull
cdtoons                 mpl2                    vorbis
cdxl                    msa1                    vp3
cfhd                    mscc                    vp4
cinepak                 msmpeg4v1               vp5
clearvideo              msmpeg4v2               vp6
cljr                    msmpeg4v3               vp6a
cllc                    msnsiren                vp6f
comfortnoise            msp2                    vp7
cook                    msrle                   vp8
cpia                    mss1                    vp8_cuvid
cri                     mss2                    vp8_qsv
cscd                    msvideo1                vp9
cyuv                    mszh                    vp9_amf
dca                     mts2                    vp9_cuvid
dds                     mv30                    vp9_qsv
derf_dpcm               mvc1                    vplayer
dfa                     mvc2                    vqa
dfpwm                   mvdv                    vqc
dirac                   mvha                    vvc
dnxhd                   mwsc                    vvc_qsv
dolby_e                 mxpeg                   wady_dpcm
dpx                     nellymoser              wavarc
dsd_lsbf                notchlc                 wavpack
dsd_lsbf_planar         nuv                     wbmp
dsd_msbf                on2avc                  wcmv
dsd_msbf_planar         opus                    webp
dsicinaudio             osq                     webp_anim
dsicinvideo             paf_audio               webvtt
dss_sp                  paf_video               wmalossless
dst                     pam                     wmapro
dvaudio                 pbm                     wmav1
dvbsub                  pcm_alaw                wmav2
dvdsub                  pcm_bluray              wmavoice
dvvideo                 pcm_dvd                 wmv1
dxa                     pcm_dvda                wmv2
dxtory                  pcm_f16le               wmv3
dxv                     pcm_f24le               wmv3image
eac3                    pcm_f32be               wnv1
eacmv                   pcm_f32le               wrapped_avframe
eamad                   pcm_f64be               ws_snd1
eatgq                   pcm_f64le               xan_dpcm
eatgv                   pcm_lxf                 xan_wc3
eatqi                   pcm_mulaw               xan_wc4
eightbps                pcm_s16be               xbin
eightsvx_exp            pcm_s16be_planar        xbm
eightsvx_fib            pcm_s16le               xface
escape124               pcm_s16le_planar        xl
escape130               pcm_s24be               xma1
evrc                    pcm_s24daud             xma2
exr                     pcm_s24le               xpm
fastaudio               pcm_s24le_planar        xsub
ffv1                    pcm_s32be               xwd
ffvhuff                 pcm_s32le               y41p
ffwavesynth             pcm_s32le_planar        ylc
fic                     pcm_s64be               yop
fits                    pcm_s64le               yuv4
flac                    pcm_s8                  zero12v
flashsv                 pcm_s8_planar           zerocodec
flashsv2                pcm_sga                 zlib
flic                    pcm_u16be               zmbv

Enabled encoders:
a64multi                hevc_amf                pcm_s24le_planar
a64multi5               hevc_d3d12va            pcm_s32be
aac                     hevc_mf                 pcm_s32le
aac_mf                  hevc_nvenc              pcm_s32le_planar
ac3                     hevc_qsv                pcm_s64be
ac3_fixed               hevc_vaapi              pcm_s64le
ac3_mf                  hevc_vulkan             pcm_s8
adpcm_adx               huffyuv                 pcm_s8_planar
adpcm_argo              jpeg2000                pcm_u16be
adpcm_g722              jpegls                  pcm_u16le
adpcm_g726              libaom_av1              pcm_u24be
adpcm_g726le            libcodec2               pcm_u24le
adpcm_ima_alp           libgsm                  pcm_u32be
adpcm_ima_amv           libgsm_ms               pcm_u32le
adpcm_ima_apm           libilbc                 pcm_u8
adpcm_ima_qt            libjxl                  pcm_vidc
adpcm_ima_ssi           libjxl_anim             pcx
adpcm_ima_wav           liblc3                  pdv
adpcm_ima_ws            libmp3lame              pfm
adpcm_ms                liboapv                 pgm
adpcm_swf               libopencore_amrnb       pgmyuv
adpcm_yamaha            libopenjpeg             phm
alac                    libopus                 png
alias_pix               librav1e                ppm
amv                     libshine                prores
anull                   libspeex                prores_aw
apng                    libsvtav1               prores_ks
aptx                    libsvtjpegxs            prores_ks_vulkan
aptx_hd                 libtheora               qoi
apv_vulkan              libtwolame              qtrle
ass                     libvo_amrwbenc          r10k
asv1                    libvorbis               r210
asv2                    libvpx_vp8              ra_144
av1_amf                 libvpx_vp9              rawvideo
av1_d3d12va             libvvenc                roq
av1_mf                  libwebp                 roq_dpcm
av1_nvenc               libwebp_anim            rpza
av1_qsv                 libx264                 rv10
av1_vaapi               libx264rgb              rv20
av1_vulkan              libx265                 s302m
avrp                    libxavs2                sbc
avui                    libxeve                 sgi
bitpacked               libxvid                 smc
bmp                     ljpeg                   snow
cfhd                    magicyuv                speedhq
cinepak                 mjpeg                   srt
cljr                    mjpeg_qsv               ssa
comfortnoise            mjpeg_vaapi             subrip
dca                     mlp                     sunrast
dfpwm                   movtext                 svq1
dnxhd                   mp2                     targa
dpx                     mp2fixed                text
dsd_msbf                mp3_mf                  tiff
dvbsub                  mpeg1video              truehd
dvdsub                  mpeg2_qsv               tta
dvvideo                 mpeg2_vaapi             ttml
dxv                     mpeg2video              utvideo
eac3                    mpeg4                   v210
exr                     msmpeg4v2               vbn
ffv1                    msmpeg4v3               vc2
ffv1_vulkan             msrle                   vnull
ffvhuff                 msvideo1                vorbis
fits                    nellymoser              vp8_vaapi
flac                    opus                    vp9_qsv
flashsv                 pam                     vp9_vaapi
flashsv2                pbm                     wavpack
flv                     pcm_alaw                wbmp
g723_1                  pcm_bluray              webvtt
gif                     pcm_dvd                 wmav1
h261                    pcm_f32be               wmav2
h263                    pcm_f32le               wmv1
h263p                   pcm_f64be               wmv2
h264_amf                pcm_f64le               wrapped_avframe
h264_d3d12va            pcm_mulaw               xbm
h264_mf                 pcm_s16be               xface
h264_nvenc              pcm_s16be_planar        xsub
h264_qsv                pcm_s16le               xwd
h264_vaapi              pcm_s16le_planar        y41p
h264_vulkan             pcm_s24be               yuv4
hap                     pcm_s24daud             zlib
hdr                     pcm_s24le               zmbv

Enabled hwaccels:
apv_vulkan              hevc_nvdec              vc1_nvdec
av1_d3d11va             hevc_nvdec_cuarray      vc1_nvdec_cuarray
av1_d3d11va2            hevc_vaapi              vc1_vaapi
av1_d3d12va             hevc_vulkan             vp8_nvdec
av1_dxva2               mjpeg_nvdec             vp8_nvdec_cuarray
av1_nvdec               mjpeg_vaapi             vp8_vaapi
av1_nvdec_cuarray       mpeg1_nvdec             vp9_d3d11va
av1_vaapi               mpeg1_nvdec_cuarray     vp9_d3d11va2
av1_vulkan              mpeg2_d3d11va           vp9_d3d12va
dpx_vulkan              mpeg2_d3d11va2          vp9_dxva2
ffv1_vulkan             mpeg2_d3d12va           vp9_nvdec
h263_vaapi              mpeg2_dxva2             vp9_nvdec_cuarray
h264_d3d11va            mpeg2_nvdec             vp9_vaapi
h264_d3d11va2           mpeg2_nvdec_cuarray     vp9_vulkan
h264_d3d12va            mpeg2_vaapi             vvc_vaapi
h264_dxva2              mpeg4_nvdec             wmv3_d3d11va
h264_nvdec              mpeg4_nvdec_cuarray     wmv3_d3d11va2
h264_nvdec_cuarray      mpeg4_vaapi             wmv3_d3d12va
h264_vaapi              prores_raw_vulkan       wmv3_dxva2
h264_vulkan             prores_vulkan           wmv3_nvdec
hevc_d3d11va            vc1_d3d11va             wmv3_nvdec_cuarray
hevc_d3d11va2           vc1_d3d11va2            wmv3_vaapi
hevc_d3d12va            vc1_d3d12va
hevc_dxva2              vc1_dxva2

Enabled parsers:
aac                     dvdsub                  mpegaudio
aac_latm                evc                     mpegvideo
ac3                     ffv1                    opus
adx                     flac                    png
ahx                     ftr                     pnm
amr                     g723_1                  prores
apv                     g729                    prores_raw
av1                     gif                     qoi
avs2                    gsm                     rv34
avs3                    h261                    sbc
bmp                     h263                    sipr
cavsvideo               h264                    tak
cook                    hdr                     vc1
cri                     hevc                    vorbis
dca                     ipu                     vp3
dirac                   jpeg2000                vp8
dnxhd                   jpegxl                  vp9
dnxuc                   jpegxs                  vvc
dolby_e                 lcevc                   webp
dpx                     misc4                   xbm
dvaudio                 mjpeg                   xma
dvbsub                  mlp                     xwd
dvd_nav                 mpeg4video

Enabled demuxers:
aa                      ico                     pcm_f32le
aac                     idcin                   pcm_f64be
aax                     idf                     pcm_f64le
ac3                     iff                     pcm_mulaw
ac4                     ifv                     pcm_s16be
ace                     ilbc                    pcm_s16le
acm                     image2                  pcm_s24be
act                     image2_alias_pix        pcm_s24le
adf                     image2_brender_pix      pcm_s32be
adp                     image2pipe              pcm_s32le
ads                     image_bmp_pipe          pcm_s8
adx                     image_cri_pipe          pcm_u16be
aea                     image_dds_pipe          pcm_u16le
afc                     image_dpx_pipe          pcm_u24be
aiff                    image_exr_pipe          pcm_u24le
aix                     image_gem_pipe          pcm_u32be
alp                     image_gif_pipe          pcm_u32le
amr                     image_hdr_pipe          pcm_u8
amrnb                   image_j2k_pipe          pcm_vidc
amrwb                   image_jpeg_pipe         pdv
anm                     image_jpegls_pipe       pjs
apac                    image_jpegxl_pipe       pmp
apc                     image_jpegxs_pipe       pp_bnk
ape                     image_pam_pipe          pva
apm                     image_pbm_pipe          pvf
apng                    image_pcx_pipe          qcp
aptx                    image_pfm_pipe          qoa
aptx_hd                 image_pgm_pipe          r3d
apv                     image_pgmyuv_pipe       rawvideo
aqtitle                 image_pgx_pipe          rcwt
argo_asf                image_phm_pipe          realtext
argo_brp                image_photocd_pipe      redspark
argo_cvg                image_pictor_pipe       rka
asf                     image_png_pipe          rl2
asf_o                   image_ppm_pipe          rm
ass                     image_psd_pipe          roq
ast                     image_qdraw_pipe        rpl
astc                    image_qoi_pipe          rsd
au                      image_sgi_pipe          rso
av1                     image_sunrast_pipe      rtp
avi                     image_svg_pipe          rtsp
avisynth                image_tiff_pipe         s337m
avr                     image_vbn_pipe          sami
avs                     image_webp_pipe         sap
avs2                    image_xbm_pipe          sbc
avs3                    image_xpm_pipe          sbg
bethsoftvid             image_xwd_pipe          scc
bfi                     imf                     scd
bfstm                   ingenient               sdns
bink                    ipmovie                 sdp
binka                   ipu                     sdr2
bintext                 ircam                   sds
bit                     iss                     sdx
bitpacked               iv8                     segafilm
bmv                     ivf                     ser
boa                     ivr                     sga
bonk                    jacosub                 shorten
brstm                   jpegxl_anim             siff
c93                     jv                      simbiosis_imx
caf                     ktx                     sln
cavsvideo               kux                     smacker
cdg                     kvag                    smjpeg
cdxl                    laf                     smush
cine                    lc3                     sol
codec2                  libgme                  sox
codec2raw               libmodplug              spdif
concat                  libopenmpt              srt
dash                    live_flv                stl
data                    lmlm4                   str
daud                    loas                    subviewer
dcstr                   lrc                     subviewer1
derf                    luodat                  sup
dfa                     lvf                     svag
dfpwm                   lxf                     svs
dhav                    m4v                     swf
dirac                   matroska                tak
dnxhd                   mca                     tedcaptions
dsf                     mcc                     thp
dsicin                  mgsts                   threedostr
dss                     microdvd                tiertexseq
dts                     mjpeg                   tmv
dtshd                   mjpeg_2000              truehd
dv                      mlp                     tta
dvbsub                  mlv                     tty
dvbtxt                  mm                      txd
dvdvideo                mmf                     ty
dxa                     mods                    usm
ea                      moflex                  v210
ea_cdata                mov                     v210x
eac3                    mp3                     vag
epaf                    mpc                     vc1
evc                     mpc8                    vc1t
ffmetadata              mpegps                  vividas
filmstrip               mpegts                  vivo
fits                    mpegtsraw               vmd
flac                    mpegvideo               vobsub
flic                    mpjpeg                  voc
flv                     mpl2                    vpk
fourxm                  mpsub                   vplayer
frm                     msf                     vqf
fsb                     msnwc_tcp               vvc
fwse                    msp                     w64
g722                    mtaf                    wady
g723_1                  mtv                     wav
g726                    musx                    wavarc
g726le                  mv                      wc3
g728                    mvi                     webm_dash_manifest
g729                    mvr                     webp_anim
gdv                     mxf                     webvtt
genh                    mxg                     wsaud
gif                     nc                      wsd
gsm                     nistsphere              wsvqa
gxf                     nsp                     wtv
h261                    nsv                     wv
h263                    nut                     wve
h264                    nuv                     xa
hca                     obu                     xbin
hcom                    ogg                     xmd
hevc                    oma                     xmv
hls                     osq                     xvag
hnm                     paf                     xwma
hxvs                    pcm_alaw                yop
iamf                    pcm_f32be               yuv4mpegpipe

Enabled muxers:
a64                     h263                    pcm_s16le
ac3                     h264                    pcm_s24be
ac4                     hash                    pcm_s24le
adts                    hds                     pcm_s32be
adx                     hevc                    pcm_s32le
aea                     hls                     pcm_s8
aiff                    iamf                    pcm_u16be
alp                     ico                     pcm_u16le
amr                     ilbc                    pcm_u24be
amv                     image2                  pcm_u24le
apm                     image2pipe              pcm_u32be
apng                    ipod                    pcm_u32le
aptx                    ircam                   pcm_u8
aptx_hd                 ismv                    pcm_vidc
apv                     iterm2                  pdv
argo_asf                ivf                     psp
argo_cvg                jacosub                 rawvideo
asf                     ktx                     rcwt
asf_stream              kvag                    rm
ass                     latm                    roq
ast                     lc3                     rso
astc                    lrc                     rtp
au                      m4v                     rtp_mpegts
avi                     matroska                rtsp
avif                    matroska_audio          sap
avm2                    mcc                     sbc
avs2                    md5                     scc
avs3                    microdvd                segafilm
bit                     mjpeg                   segment
caf                     mkvtimestamp_v2         smjpeg
cavsvideo               mlp                     smoothstreaming
chromaprint             mmf                     sox
codec2                  mov                     spdif
codec2raw               mp2                     spx
crc                     mp3                     srt
dash                    mp4                     stream_segment
data                    mpeg1system             streamhash
daud                    mpeg1vcd                sup
dfpwm                   mpeg1video              swf
dirac                   mpeg2dvd                tee
dnxhd                   mpeg2svcd               tg2
dts                     mpeg2video              tgp
dv                      mpeg2vob                truehd
eac3                    mpegts                  tta
evc                     mpjpeg                  ttml
f4v                     mxf                     uncodedframecrc
ffmetadata              mxf_d10                 vc1
fifo                    mxf_opatom              vc1t
filmstrip               null                    voc
fits                    nut                     vvc
flac                    obu                     w64
flv                     oga                     wav
framecrc                ogg                     webm
framehash               ogv                     webm_chunk
framemd5                oma                     webm_dash_manifest
g722                    opus                    webp
g723_1                  pcm_alaw                webvtt
g726                    pcm_f32be               whip
g726le                  pcm_f32le               wsaud
gif                     pcm_f64be               wtv
gsm                     pcm_f64le               wv
gxf                     pcm_mulaw               yuv4mpegpipe
h261                    pcm_s16be

Enabled protocols:
async                   http                    rtmp
bluray                  httpproxy               rtmpe
cache                   https                   rtmps
concat                  icecast                 rtmpt
concatf                 ipfs_gateway            rtmpte
crypto                  ipns_gateway            rtmpts
data                    librist                 rtp
dtls                    libsrt                  srtp
fd                      libssh                  subfile
ffrtmpcrypt             libzmq                  tcp
ffrtmphttp              md5                     tee
file                    mmsh                    tls
ftp                     mmst                    udp
gopher                  pipe                    udplite
gophers                 prompeg

Enabled filters:
a3dscope                dedot                   perms
aap                     deesser                 perspective
abench                  deflate                 phase
abitscope               deflicker               photosensitivity
acompressor             deinterlace_d3d12       pixdesctest
acontrast               deinterlace_qsv         pixelize
acopy                   deinterlace_vaapi       pixscope
acrossfade              dejudder                pp7
acrossover              delogo                  premultiply
acrusher                denoise_vaapi           premultiply_dynamic
acue                    deshake                 prewitt
addroi                  deshake_opencl          prewitt_opencl
adeclick                despill                 procamp_vaapi
adeclip                 detelecine              program_opencl
adecorrelate            dialoguenhance          pseudocolor
adelay                  dilation                psnr
adenorm                 dilation_opencl         pullup
aderivative             displace                qp
adrawgraph              doubleweave             qrencode
adrc                    drawbox                 qrencodesrc
adynamicequalizer       drawbox_vaapi           quirc
adynamicsmooth          drawgraph               random
aecho                   drawgrid                readeia608
aemphasis               drawtext                readvitc
aeval                   drawvg                  realtime
aevalsrc                drmeter                 remap
aexciter                dynaudnorm              remap_opencl
afade                   earwax                  removegrain
afdelaysrc              ebur128                 removelogo
afftdn                  edgedetect              repeatfields
afftfilt                elbg                    replaygain
afir                    entropy                 reverse
afireqsrc               epx                     rgbashift
afirsrc                 eq                      rgbtestsrc
aformat                 equalizer               roberts
afreqshift              erosion                 roberts_opencl
afwtdn                  erosion_opencl          rotate
agate                   estdif                  rubberband
agraphmonitor           exposure                sab
ahistogram              extractplanes           scale
aiir                    extrastereo             scale2ref
aintegral               fade                    scale_cuda
ainterleave             feedback                scale_d3d11
alatency                fftdnoiz                scale_d3d12
alimiter                fftfilt                 scale_qsv
allpass                 field                   scale_vaapi
allrgb                  fieldhint               scale_vulkan
allyuv                  fieldmatch              scdet
aloop                   fieldorder              scdet_vulkan
alphaextract            fillborders             scharr
alphamerge              find_rect               scroll
amerge                  firequalizer            segment
ametadata               flanger                 select
amf_capture             flip_vulkan             selectivecolor
amix                    flite                   sendcmd
amovie                  floodfill               separatefields
amplify                 format                  setdar
amultiply               fps                     setfield
anequalizer             framepack               setparams
anlmdn                  framerate               setpts
anlmf                   framestep               setrange
anlms                   frc_amf                 setsar
anoisesrc               freezedetect            settb
anull                   freezeframes            sharpness_vaapi
anullsink               frei0r                  shear
anullsrc                frei0r_src              showcqt
apad                    fruc_vulkan             showcwt
aperms                  fspp                    showfreqs
aphasemeter             fsync                   showinfo
aphaser                 gblur                   showpalette
aphaseshift             gblur_vulkan            showspatial
apsnr                   geq                     showspectrum
apsyclip                gfxcapture              showspectrumpic
apulsator               gradfun                 showvolume
arealtime               gradients               showwaves
aresample               graphmonitor            showwavespic
areverse                grayworld               shuffleframes
arls                    greyedge                shufflepixels
arnndn                  guided                  shuffleplanes
asdr                    haas                    sidechaincompress
asegment                haldclut                sidechaingate
aselect                 haldclutsrc             sidedata
asendcmd                hdcd                    sierpinski
asetnsamples            headphone               signalstats
asetpts                 hflip                   signature
asetrate                hflip_vulkan            silencedetect
asettb                  highpass                silenceremove
ashowinfo               highshelf               sinc
asidedata               hilbert                 sine
asisdr                  histeq                  siti
asoftclip               histogram               smartblur
aspectralstats          hqdn3d                  smptebars
asplit                  hqx                     smptehdbars
ass                     hstack                  sobel
astats                  hstack_qsv              sobel_opencl
astreamselect           hstack_vaapi            sofalizer
asubboost               hsvhold                 spectrumsynth
asubcut                 hsvkey                  speechnorm
asupercut               hue                     split
asuperpass              huesaturation           spp
asuperstop              hwdownload              sr_amf
atadenoise              hwmap                   ssim
atempo                  hwupload                ssim360
atilt                   hwupload_cuda           stereo3d
atrim                   hysteresis              stereotools
avectorscope            iccdetect               stereowiden
avgblur                 iccgen                  streamselect
avgblur_opencl          identity                subtitles
avgblur_vulkan          idet                    super2xsai
avsynctest              il                      superequalizer
axcorrelate             inflate                 surround
azmq                    interlace               swaprect
backgroundkey           interlace_vulkan        swapuv
bandpass                interleave              tblend
bandreject              join                    telecine
bass                    kerndeint               testsrc
bbox                    kirsch                  testsrc2
bench                   ladspa                  thistogram
bilateral               lagfun                  threshold
bilateral_cuda          latency                 thumbnail
biquad                  latticepal              thumbnail_cuda
bitplanenoise           lenscorrection          tile
blackdetect             lensfun                 tiltandshift
blackdetect_vulkan      libplacebo              tiltshelf
blackframe              libvmaf                 tinterlace
blend                   life                    tlut2
blend_vulkan            limitdiff               tmedian
blockdetect             limiter                 tmidequalizer
blurdetect              loop                    tmix
bm3d                    loudnorm                tonemap
boxblur                 lowpass                 tonemap_opencl
boxblur_opencl          lowshelf                tonemap_vaapi
bs2b                    lumakey                 tpad
bwdif                   lut                     transpose
bwdif_cuda              lut1d                   transpose_cuda
bwdif_vulkan            lut2                    transpose_opencl
cas                     lut3d                   transpose_vaapi
ccrepack                lutrgb                  transpose_vulkan
cellauto                lutyuv                  treble
channelmap              mandelbrot              tremolo
channelsplit            maskedclamp             trim
chorus                  maskedmax               unpremultiply
chromaber_vulkan        maskedmerge             unsharp
chromahold              maskedmin               unsharp_opencl
chromakey               maskedthreshold         untile
chromakey_cuda          maskfun                 uspp
chromanr                mcdeint                 v360
chromashift             mcompand                v360_vulkan
ciescope                median                  vaguedenoiser
codecview               mergeplanes             varblur
color                   mestimate               vectorscope
color_vulkan            mestimate_d3d12         vflip
colorbalance            metadata                vflip_vulkan
colorchannelmixer       midequalizer            vfrdet
colorchart              minterpolate            vibrance
colorcontrast           mix                     vibrato
colorcorrect            monochrome              vidstabdetect
colordetect             morpho                  vidstabtransform
colorhold               movie                   vif
colorize                mpdecimate              vignette
colorkey                mptestsrc               virtualbass
colorkey_opencl         msad                    vmafmotion
colorlevels             multiply                volume
colormap                negate                  volumedetect
colormatrix             nlmeans                 vpp_amf
colorspace              nlmeans_opencl          vpp_qsv
colorspace_cuda         nlmeans_vulkan          vqe_amf
colorspectrum           nnedi                   vstack
colortemperature        noformat                vstack_qsv
compand                 noise                   vstack_vaapi
compensationdelay       normalize               w3fdif
concat                  null                    waveform
convolution             nullsink                weave
convolution_opencl      nullsrc                 whisper
convolve                openclsrc               xbr
copy                    oscilloscope            xcorrelate
corr                    overlay                 xfade
cover_rect              overlay_cuda            xfade_opencl
crop                    overlay_opencl          xfade_vulkan
cropdetect              overlay_qsv             xmedian
crossfeed               overlay_vaapi           xpsnr
crystalizer             overlay_vulkan          xstack
cue                     owdenoise               xstack_qsv
curves                  pad                     xstack_vaapi
datascope               pad_cuda                yadif
dblur                   pad_opencl              yadif_cuda
dcshift                 pad_vaapi               yaepblur
dctdnoiz                pal100bars              yuvtestsrc
ddagrab                 pal75bars               zmq
deband                  palettegen              zoneplate
deblock                 paletteuse              zoompan
decimate                pan                     zscale
deconvolve              perlin

Enabled bsfs:
aac_adtstoasc           h264_metadata           pcm_rechunk
ahx_to_mp2              h264_mp4toannexb        pgs_frame_merge
apv_metadata            h264_redundant_pps      prores_metadata
av1_frame_merge         hapqa_extract           remove_extradata
av1_frame_split         hevc_metadata           setts
av1_metadata            hevc_mp4toannexb        showinfo
chomp                   imx_dump_header         smpte436m_to_eia608
dca_core                lcevc_merge             text2movsub
dovi_rpu                lcevc_metadata          trace_headers
dovi_split              media100_to_mjpegb      trim
dts2pts                 mjpeg2jpeg              truehd_core
dump_extradata          mjpega_dump_header      vp9_metadata
dv_error_marker         mov2textsub             vp9_raw_reorder
eac3_core               mpeg2_metadata          vp9_superframe
eia608_to_smpte436m     mpeg4_unpack_bframes    vp9_superframe_split
evc_frame_merge         noise                   vvc_metadata
extract_extradata       null                    vvc_mp4toannexb
filter_units            opus_metadata

Enabled indevs:
dshow                   lavfi                   openal
gdigrab                 libcdio                 vfwcap

Enabled outdevs:
caca

git-full external libraries' versions: 

AMF v1.5.2-3-ga4c8f39
aom v3.15.0-42-g208e9c49db
aribb24 v1.0.3-5-g5e9be27
aribcaption 1.1.2
AviSynthPlus v3.7.5-383-g5c82777b
bs2b 3.1.0
cairo 1.18.5
chromaprint 1.6.1
codec2 1.2.0-108-g310777b1
dav1d 1.5.4-12-gc2e9c9e5
davs2 1.7-1-gb41cf11
dvdnav 7.0.0-18-g8147ccd
dvdread 7.1.1-125-g0f30df5
ffnvcodec n13.1.15.0-1-geddcea9
flite v2.2-55-g6c9f20d
freetype VER-2-14-3
frei0r v3.5.0-4-g5585a36
fribidi v1.0.17
gsm 1.0.24
harfbuzz 14.4.0-115-g240a7d7b
ladspa-sdk 1.17
lame 3.100
lc3 1.1.3
lcms2 2.16
lensfun v0.3.95-2018-g84401750
libass 0.17.5-11-gf61db56
libbluray 1.5.1
libcaca 0.99.
libcdio-paranoia 10.2
libgme 0.6.6
libilbc v3.0.4-346-g6adb26d4a4
libjxl v0.12-snapshot-16-g7741c8ce
libmodplug 0.8.9.0
libopencore-amrnb 0.1.6
libopencore-amrwb 0.1.6
libplacebo v7.360.0-131-ge2972fd
libsoxr 0.1.3
libssh 0.12.2
libtheora v1.2.0
libwebp v1.6.0-261-gea0c620
openal-soft 1.25.2-48-g7532541
openapv v0.3.0.0-9-ga5312e4
OpenCL-Headers 
openjpeg2 2.5.4
openmpt libopenmpt-0.6.30-3-g6c0a06bc
opus v1.6.1-68-g503d81b1
qrencode 4.1.1
quirc 1.2
rav1e p20250624-8-g31435de
rist 0.2.20
rubberband v4.0.0
SDL release-2.32.0-234-gb90ac9502
shine 3.1.1
snappy 1.2.2
speex Speex-1.2.1-51-g0589522
srt v1.5.7-8-gff8ab25
SVT-AV1 v4.2.0-cqp-extended-75-g167138974
SVT-JPEG-XS v0.9.0-100-gf9c82c1
twolame 0.4.0
uavs3d 1.2
VAAPI 2.25.0.
vidstab v1.1.2-214-ge2445c4
vmaf v3.2.1-8-g86da14d0
vo-amrwbenc 0.1.3
vorbis v1.3.7-37-g1b75110b
VPL 2.17
vpx v1.17.0-61-g5e680f308
vulkan-loader v1.4.363-2-gbad58e02
vvenc v1.14.0-184-gb1cfb2a
whisper.cpp 1.9.4
x264 v0.165.3223
x265 4.3-45-g116b875
xavs2 1.4
xevd 0.7.0
xeve 0.7.0
xvid v1.3.7
zeromq 4.3.5
zimg release-3.0.6-253-g67e0603
zvbi v0.2.45

