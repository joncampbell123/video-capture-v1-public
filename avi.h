/* AVI file structure definitions. Also defines the BITMAPINFOHEADER and WAVE format structures.
 * (C) 2008-2015 Jonathan Campbell.
 * Alternate copy for open source video capture project.
 */
 
#ifndef __VIDEOCAP_VIDEOCAP_AVI_H
#define __VIDEOCAP_VIDEOCAP_AVI_H

#include <stdint.h>

typedef uint32_t avi_fourcc_t;
#define avi_fourcc_const(a,b,c,d)	( (((uint32_t)(a)) << 0U) | (((uint32_t)(b)) << 8U) | (((uint32_t)(c)) << 16U) | (((uint32_t)(d)) << 24U) )

#define avi_riff_AVI			avi_fourcc_const('A','V','I',' ')
#define avi_riff_movi			avi_fourcc_const('m','o','v','i')
#define avi_riff_hdrl			avi_fourcc_const('h','d','r','l')
#define avi_riff_idx1			avi_fourcc_const('i','d','x','1')
#define avi_riff_indx			avi_fourcc_const('i','n','d','x')
#define avi_riff_avih			avi_fourcc_const('a','v','i','h')
#define avi_riff_strl			avi_fourcc_const('s','t','r','l')
#define avi_riff_strh			avi_fourcc_const('s','t','r','h')
#define avi_riff_strf			avi_fourcc_const('s','t','r','f')
#define avi_riff_vprp			avi_fourcc_const('v','p','r','p')
#define avi_riff_dmlh			avi_fourcc_const('d','m','l','h')
#define avi_riff_odml			avi_fourcc_const('o','d','m','l')

#define avi_fccType_audio		avi_fourcc_const('a','u','d','s')
#define avi_fccType_video		avi_fourcc_const('v','i','d','s')

/* AVI file struct: AVIMAINHEADER */
typedef struct {
	/* FOURCC */
	/* uint32_t cb */
	uint32_t        dwMicroSecPerFrame;
	uint32_t        dwMaxBytesPerSec;
	uint32_t        dwPaddingGranularity;
	uint32_t        dwFlags;
	uint32_t        dwTotalFrames;
	uint32_t        dwInitialFrames;
	uint32_t        dwStreams;
	uint32_t        dwSuggestedBufferSize;
	uint32_t        dwWidth;
	uint32_t        dwHeight;
	uint32_t        dwReserved[4];
} __attribute__((packed)) riff_avih_AVIMAINHEADER;

#define riff_avih_AVIMAINHEADER_flags_HASINDEX				0x00000010UL
#define riff_avih_AVIMAINHEADER_flags_MUSTUSEINDEX			0x00000020UL
#define riff_avih_AVIMAINHEADER_flags_ISINTERLEAVED			0x00000100UL
#define riff_avih_AVIMAINHEADER_flags_TRUSTCKTYPE			0x00000800UL
#define riff_avih_AVIMAINHEADER_flags_WASCAPTUREFILE			0x00010000UL
#define riff_avih_AVIMAINHEADER_flags_COPYRIGHTED			0x00020000UL

/* AVI file struct: AVISTREAMHEADER */
typedef struct {
	/* FOURCC */
	/* uint32_t cb */
	avi_fourcc_t    fccType;
	avi_fourcc_t    fccHandler;
	uint32_t        dwFlags;
	uint16_t        wPriority;
	uint16_t        wLanguage;
	uint32_t        dwInitialFrames;
	uint32_t        dwScale;
	uint32_t        dwRate;
	uint32_t        dwStart;
	uint32_t        dwLength;
	uint32_t        dwSuggestedBufferSize;
	uint32_t        dwQuality;
	uint32_t        dwSampleSize;
	struct {
		int16_t left;
		int16_t top;
		int16_t right;
		int16_t bottom;
	} __attribute__((packed)) rcFrame;
} __attribute__((packed)) riff_strh_AVISTREAMHEADER;

#define riff_strh_AVISTREAMHEADER_flags_DISABLED			0x00000001UL
#define riff_strh_AVISTREAMHEADER_flags_VIDEO_PALCHANGES		0x00010000UL

/* AVIPALCHANGE */
typedef struct {
	uint8_t		bFirstEntry;
	uint8_t		bNumEntries;
	uint16_t	wFlags;
	/* PALETTEENTRY[] */
} __attribute__((packed)) riff_AVIPALCHANGE_header;

/* AVI palette entry */
typedef struct {
	uint8_t		peRed,peGreen,peBlue,peFlags;
} __attribute__((packed)) riff_AVIPALCHANGE_PALETTEENTRY;

#define riff_AVIPALCHANGE_PALETTEENTRY_flags_PC_RESERVED		0x01U
#define riff_AVIPALCHANGE_PALETTEENTRY_flags_PC_EXPLICIT		0x02U
#define riff_AVIPALCHANGE_PALETTEENTRY_flags_PC_NOCOLLAPSE		0x04U

/* AVIOLDINDEX (one element of the structure) */
typedef struct {
	uint32_t        dwChunkId;
	uint32_t        dwFlags;
	uint32_t        dwOffset;
	uint32_t        dwSize;
} __attribute__((packed)) riff_idx1_AVIOLDINDEX;

/* AVIOLDINDEX chunk IDs. NOTE that this chunk ID makes the last two bytes of dwChunkId (the upper 16 bits) */
/* NOTICE due to little Endian byte order the string is typed in reverse here */
#define riff_idx1_AVIOLDINDEX_chunkid_type_mask				0xFFFF0000UL
#define riff_idx1_AVIOLDINDEX_chunkid_stream_index_mask			0x0000FFFFUL
#define riff_idx1_AVIOLDINDEX_chunkid_uncompressed_videoframe		avi_fourcc_const(0,0,'d','b')
#define riff_idx1_AVIOLDINDEX_chunkid_compressed_videoframe		avi_fourcc_const(0,0,'d','c')
#define riff_idx1_AVIOLDINDEX_chunkid_palette_change			avi_fourcc_const(0,0,'p','c')
#define riff_idx1_AVIOLDINDEX_chunkid_audio_data			avi_fourcc_const(0,0,'w','b')

#define riff_idx1_AVIOLDINDEX_flags_LIST				0x00000001UL
#define riff_idx1_AVIOLDINDEX_flags_KEYFRAME				0x00000010UL
#define riff_idx1_AVIOLDINDEX_flags_FIRSTPART				0x00000020UL
#define riff_idx1_AVIOLDINDEX_flags_LASTPART				0x00000040UL
#define riff_idx1_AVIOLDINDEX_flags_NO_TIME				0x00000100UL

/* AVIMETAINDEX (a meta-structure for all the variations in indx and nnix chunks) */
typedef struct {
/*	FOURCC          fcc;
	UINT            cb; */
	uint16_t        wLongsPerEntry;
	uint8_t         bIndexSubType;
	uint8_t         bIndexType;
	uint32_t        nEntriesInUse;
	uint32_t        dwChunkId;
	uint32_t        dwReserved[3];
/*	uint32_t           adwIndex[]; */
} __attribute__((packed)) riff_indx_AVIMETAINDEX;

#define riff_indx_type_AVI_INDEX_OF_INDEXES				0x00
#define riff_indx_type_AVI_INDEX_OF_CHUNKS				0x01
#define riff_indx_type_AVI_INDEX_OF_TIMED_CHUNKS			0x02
#define riff_indx_type_AVI_INDEX_OF_SUB_2FIELD				0x03
#define riff_indx_type_AVI_INDEX_IS_DATA				0x80

#define riff_indx_subtype_AVI_INDEX_SUB_DEFAULT				0x00
#define riff_indx_subtype_AVI_INDEX_SUB_2FIELD				0x01

/* AVISUPERINDEX */
typedef struct {
/*	FOURCC          fcc;
	UINT            cb; */
	uint16_t        wLongsPerEntry;
	uint8_t         bIndexSubType;
	uint8_t         bIndexType;
	uint32_t        nEntriesInUse;
	uint32_t        dwChunkId;
	uint32_t        dwReserved[3];
/*	AVISUPERINDEXentry[] */
} __attribute__((packed)) riff_indx_AVISUPERINDEX;

typedef struct {
	uint64_t        qwOffset;
	uint32_t        dwSize;
	uint32_t        dwDuration;
} __attribute__((packed)) riff_indx_AVISUPERINDEX_entry;

/* AVISTDINDEX */
typedef struct {
/*	FOURCC          fcc;
	UINT            cb; */
	uint16_t        wLongsPerEntry;
	uint8_t         bIndexSubType;
	uint8_t         bIndexType;
	uint32_t        nEntriesInUse;
	uint32_t        dwChunkId;
	uint64_t	qwBaseOffset;
	uint32_t	dwReserved_3;
/*	AVISTDINDEXentry[] */
} __attribute__((packed)) riff_indx_AVISTDINDEX;

typedef struct {
	uint32_t        dwOffset;		/* relative to qwBaseOffset */
	uint32_t        dwSize;			/* bit 31 is set if delta frame */
} __attribute__((packed)) riff_indx_AVISTDINDEX_entry;

typedef struct {
	uint32_t        CompressedBMHeight;
	uint32_t        CompressedBMWidth;
	uint32_t        ValidBMHeight;
	uint32_t        ValidBMWidth;
	uint32_t        ValidBMXOffset;
	uint32_t        ValidBMYOffset;
	uint32_t        VideoXOffsetInT;
	uint32_t        VideoYValidStartLine;
} __attribute__((packed)) riff_vprp_VIDEO_FIELD_DESC;

/* vprp chunk */
typedef struct {
	uint32_t        VideoFormatToken;
	uint32_t        VideoStandard;
	uint32_t        dwVerticalRefreshRate;
	uint32_t        dwHTotalInT;
	uint32_t        dwVTotalInLines;
	uint32_t        dwFrameAspectRatio;
	uint32_t        dwFrameWidthInPixels;
	uint32_t        dwFrameHeightInLines;
	uint32_t        nbFieldPerFrame;
/*	riff_vprp_VIDEO_FIELD_DESC FieldInfo[nbFieldPerFrame]; */
} __attribute__((packed)) riff_vprp_VideoPropHeader;

/* LIST:odml dmlh chunk */
typedef struct {
	uint32_t	dwTotalFrames;
} __attribute__((packed)) riff_odml_dmlh_ODMLExtendedAVIHeader;

/* --------------------------------------- WAVE structures (used in AVI) --------------------------- */

typedef struct {
	uint16_t  wFormatTag;
	uint16_t  nChannels;
	uint32_t  nSamplesPerSec;
	uint32_t  nAvgBytesPerSec;
	uint16_t  nBlockAlign;
	uint16_t  wBitsPerSample;
} __attribute__((packed)) windows_WAVEFORMAT;

typedef struct {
	uint16_t  wFormatTag;
	uint16_t  nChannels;
	uint32_t  nSamplesPerSec;
	uint32_t  nAvgBytesPerSec;
	uint16_t  nBlockAlign;
	uint16_t  wBitsPerSample;
	uint16_t  cbSize;
} __attribute__((packed)) windows_WAVEFORMATEX;

typedef struct {
	windows_WAVEFORMATEX	wfx;
	uint16_t		wSamplesPerBlock;
	uint16_t		wNumCoef;
	uint16_t		aCoef[7*2];	/* NTS: This array is wNumCoef*2 large, for MS-ADPCM wNumCoef == 7 */
} __attribute__((packed)) windows_ADPCMWAVEFORMAT; /* sizeof() == 32 bytes */

typedef struct {
	uint32_t  a;
	uint16_t  b,c;
	uint8_t   d[2];
	uint8_t   e[6];
} __attribute__((packed)) windows_GUID;

typedef struct {
	windows_WAVEFORMATEX     Format;
	union { /* Ooookay Microsoft how do I derive meaning from THIS now? */
		uint16_t         wValidBitsPerSample;
		uint16_t         wSamplesPerBlock;
		uint16_t         wReserved;
	} Samples;
	uint32_t                 dwChannelMask;
	windows_GUID             SubFormat;
} __attribute__((packed)) windows_WAVEFORMATEXTENSIBLE;

#define windows_WAVE_FORMAT_PCM		0x0001
#define windows_WAVE_FORMAT_MS_ADPCM	0x0002
#define windows_WAVE_FORMAT_IEEE_FLOAT	0x0003

#define windows_WAVE_FORMAT_ALAW	0x0006
#define windows_WAVE_FORMAT_MULAW	0x0007

#define windows_WAVE_FORMAT_IMA_ADPCM	0x0011

#define windows_WAVE_FORMAT_EXTENSIBLE	0xFFFE

/* ---------------------------------- BITMAPINFOHEADER (used in AVI and core to the Windows GDI) ---------------- */

typedef struct {
	uint32_t        biSize; 
	int32_t         biWidth; 
	int32_t         biHeight; 
	uint16_t        biPlanes; 
	uint16_t        biBitCount; 
	uint32_t        biCompression; 
	uint32_t        biSizeImage; 
	int32_t         biXPelsPerMeter; 
	int32_t         biYPelsPerMeter; 
	uint32_t        biClrUsed; 
	uint32_t        biClrImportant; 
} __attribute__((packed)) windows_BITMAPINFOHEADER;

typedef struct {
	uint32_t	ciexyzRed;
	uint32_t	ciexyzGreen;
	uint32_t	ciexyzBlue;
} __attribute__((packed)) windows_CIEXYZTRIPLE;

typedef struct {
	uint32_t        bV4Size;
	int32_t         bV4Width;
	int32_t         bV4Height;
	uint16_t        bV4Planes;
	uint16_t        bV4BitCount;
	uint32_t        bV4V4Compression;
	uint32_t        bV4SizeImage;
	int32_t         bV4XPelsPerMeter;
	int32_t         bV4YPelsPerMeter;
	uint32_t        bV4ClrUsed;
	uint32_t        bV4ClrImportant;
	uint32_t        bV4RedMask;
	uint32_t        bV4GreenMask;
	uint32_t        bV4BlueMask;
	uint32_t        bV4AlphaMask;
	uint32_t        bV4CSType;
	windows_CIEXYZTRIPLE bV4Endpoints;
	uint32_t        bV4GammaRed;
	uint32_t        bV4GammaGreen;
	uint32_t        bV4GammaBlue;
} __attribute__((packed)) windows_BITMAPV4HEADER;

typedef struct {
	uint32_t        bV5Size;
	int32_t         bV5Width;
	int32_t         bV5Height;
	uint16_t        bV5Planes;
	uint16_t        bV5BitCount;
	uint32_t        bV5Compression;
	uint32_t        bV5SizeImage;
	int32_t         bV5XPelsPerMeter;
	int32_t         bV5YPelsPerMeter;
	uint32_t        bV5ClrUsed;
	uint32_t        bV5ClrImportant;
	uint32_t        bV5RedMask;
	uint32_t        bV5GreenMask;
	uint32_t        bV5BlueMask;
	uint32_t        bV5AlphaMask;
	uint32_t        bV5CSType;
	windows_CIEXYZTRIPLE bV5Endpoints;
	uint32_t        bV5GammaRed;
	uint32_t        bV5GammaGreen;
	uint32_t        bV5GammaBlue;
	uint32_t        bV5Intent;
	uint32_t        bV5ProfileData;
	uint32_t        bV5ProfileSize;
	uint32_t        bV5Reserved;
} __attribute__((packed)) windows_BITMAPV5HEADER;

#endif

