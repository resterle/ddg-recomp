/* Types recovered from unb/Graphic.cpp in perfect.exe (Densha de GO! FINAL, PC). */

/* One slot in CGraphic's texture table. Slots 0 and 1 are the render targets
 * used for screen effects ("m_riTable"); the rest are regular textures. */
typedef struct TEXINFO {
    IDirect3DTexture8 *pD3DT;
    unsigned int nWidth;
    unsigned int nHeight;
    D3DFORMAT nD3DF;
    unsigned int nD3DP;          /* palette number, >= 0x4000 means none assigned */
    unsigned char bDirty;
    unsigned char bDirtyPalette;
    unsigned char pad[2];
} TEXINFO;

/* Fixed size override for a texture ID. */
typedef struct TEXSIZE {
    int nID;
    unsigned short nWidth;
    unsigned short nHeight;
} TEXSIZE;

/* Pre-transformed vertices for 2D drawing (FVF 0x144 / 0x44). */
typedef struct TLVERTEX_TEX { float x; float y; float z; float rhw; unsigned int color; float u; float v; } TLVERTEX_TEX;
typedef struct TLVERTEX { float x; float y; float z; float rhw; unsigned int color; } TLVERTEX;

/* Settings read from HKCU\Software\TAITO\<game> by CGraphic::LoadConfig.
 * Field names follow the registry value names. */
typedef struct GRAPHIC_CONFIG {
    int windowed;
    int gpu;                     /* use hardware T&L if available */
    int scrwidth;
    int scrheight;
    int bbcount;
    int scrformat;
    int refrate;
    int zformat;
    int texformat;
    int texmodulate;
    int fog;
    int texsize;
    int screffect;
    int snapshot;
} GRAPHIC_CONFIG;

typedef struct CRITICAL_SECTION_ { void *DebugInfo; int LockCount; int RecursionCount; void *OwningThread; void *LockSemaphore; unsigned int SpinCount; } CRITICAL_SECTION_;

/* CGraphic methods. All are static: CGraphic has no instance, its members are globals. */
int CGraphic_LoadConfig(void);
int CGraphic_Init(void *hWnd);
void CGraphic_Term(void);
int CGraphic_Reset(void);
D3DFORMAT CGraphic_GetAdapterFormat(D3DFORMAT fmtDisplay);
D3DFORMAT CGraphic_GetDepthStencilFormat(D3DFORMAT fmtAdapter);
D3DFORMAT CGraphic_GetTextureFormat(D3DFORMAT fmtAdapter);
unsigned int CGraphic_GetRefreshRate(D3DFORMAT fmtAdapter);
unsigned int CGraphic_GetTextureModulate(void);
unsigned int CGraphic_GetMaxTextureWidth(void);
unsigned int CGraphic_GetMaxTextureHeight(void);
unsigned int CGraphic_GetBehaviorFlags(void);
int CGraphic_SetGamma(float fGamma, int bStore);
TEXSIZE *CGraphic_FindTexSize(int nID);
int CGraphic_NormalizeUV(float *pu, float *pv);
int CGraphic_SetVertex(int i, unsigned int color, float *pos, float u, float v);
int CGraphic_AddStripVertex(int i, unsigned int color, float *pos, float u, float v);
int CGraphic_SetSpriteRect(unsigned int color, float *pos, float u, float v);
int CGraphic_ClearBorders(void);
int CGraphic_SetViewport(int nType, int bApply);
int CGraphic_CreateRenderTargets(void);
void CGraphic_ReleaseRenderTargets(void);
int CGraphic_SetRenderTargetTexture(int nType);
int CGraphic_ApplyScreenEffect(void);

typedef struct D3DXVECTOR3 { float x; float y; float z; } D3DXVECTOR3;

/* Helpers and statically linked library routines used by CGraphic. */
void DebugLog(char *fmt, ...);
char *FormatString(char *fmt, ...);
char *D3DFormatToString(D3DFORMAT fmt);
char * __stdcall DXGetErrorString8A(long hr);
char * __stdcall DXGetErrorDescription8A(long hr);
D3DMATRIX * __stdcall D3DXMatrixLookAtLH(D3DMATRIX *pOut, D3DXVECTOR3 *pEye, D3DXVECTOR3 *pAt, D3DXVECTOR3 *pUp);
D3DMATRIX * __stdcall D3DXMatrixPerspectiveFovLH(D3DMATRIX *pOut, float fovy, float aspect, float zn, float zf);
long __stdcall D3DXCheckTextureRequirements(IDirect3DDevice8 *pDevice, unsigned int *pWidth, unsigned int *pHeight, unsigned int *pNumMipLevels, unsigned int Usage, D3DFORMAT *pFormat, D3DPOOL Pool);
int CGraphic_SetTexture(int nID, char nGroup, int nPalID);
int CGraphic_PrintScreen(void);
IDirect3D8 * __stdcall Direct3DCreate8(unsigned int SDKVersion);
