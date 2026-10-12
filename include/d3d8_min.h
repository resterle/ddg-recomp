/* Minimal Direct3D 8 declarations for Ghidra's C parser.
 * Vtable order follows the DirectX 8.1 SDK d3d8.h; offsets were checked
 * against call sites in perfect.exe (e.g. SetRenderState = +0xC8, Clear = +0x90). */

typedef unsigned int D3DFORMAT;
typedef unsigned int D3DDEVTYPE;
typedef unsigned int D3DPOOL;
typedef unsigned int D3DPRIMITIVETYPE;
typedef unsigned int D3DRENDERSTATETYPE;
typedef unsigned int D3DTEXTURESTAGESTATETYPE;
typedef unsigned int D3DTRANSFORMSTATETYPE;
typedef unsigned int D3DMULTISAMPLE_TYPE;
typedef unsigned int D3DSWAPEFFECT;
typedef unsigned int D3DRESOURCETYPE;
typedef unsigned int D3DBACKBUFFER_TYPE;
typedef unsigned int D3DLIGHTTYPE;

typedef struct D3DDISPLAYMODE { unsigned int Width; unsigned int Height; unsigned int RefreshRate; D3DFORMAT Format; } D3DDISPLAYMODE;
typedef struct D3DVIEWPORT8 { unsigned int X; unsigned int Y; unsigned int Width; unsigned int Height; float MinZ; float MaxZ; } D3DVIEWPORT8;
typedef struct D3DMATRIX { float m[4][4]; } D3DMATRIX;
typedef struct D3DVECTOR { float x; float y; float z; } D3DVECTOR;
typedef struct D3DCOLORVALUE { float r; float g; float b; float a; } D3DCOLORVALUE;
typedef struct D3DRECT { int x1; int y1; int x2; int y2; } D3DRECT;
typedef struct D3DLOCKED_RECT { int Pitch; void *pBits; } D3DLOCKED_RECT;
typedef struct D3DGAMMARAMP { unsigned short red[256]; unsigned short green[256]; unsigned short blue[256]; } D3DGAMMARAMP;

typedef struct D3DSURFACE_DESC {
    D3DFORMAT Format; D3DRESOURCETYPE Type; unsigned int Usage; D3DPOOL Pool;
    unsigned int Size; D3DMULTISAMPLE_TYPE MultiSampleType; unsigned int Width; unsigned int Height;
} D3DSURFACE_DESC;

typedef struct D3DLIGHT8 {
    D3DLIGHTTYPE Type; D3DCOLORVALUE Diffuse; D3DCOLORVALUE Specular; D3DCOLORVALUE Ambient;
    D3DVECTOR Position; D3DVECTOR Direction;
    float Range; float Falloff; float Attenuation0; float Attenuation1; float Attenuation2; float Theta; float Phi;
} D3DLIGHT8;

typedef struct D3DMATERIAL8 { D3DCOLORVALUE Diffuse; D3DCOLORVALUE Ambient; D3DCOLORVALUE Specular; D3DCOLORVALUE Emissive; float Power; } D3DMATERIAL8;

typedef struct D3DPRESENT_PARAMETERS {
    unsigned int BackBufferWidth; unsigned int BackBufferHeight; D3DFORMAT BackBufferFormat; unsigned int BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType; D3DSWAPEFFECT SwapEffect; void *hDeviceWindow; int Windowed;
    int EnableAutoDepthStencil; D3DFORMAT AutoDepthStencilFormat; unsigned int Flags;
    unsigned int FullScreen_RefreshRateInHz; unsigned int FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct D3DCAPS8 {
    D3DDEVTYPE DeviceType; unsigned int AdapterOrdinal; unsigned int Caps; unsigned int Caps2; unsigned int Caps3;
    unsigned int PresentationIntervals; unsigned int CursorCaps; unsigned int DevCaps; unsigned int PrimitiveMiscCaps;
    unsigned int RasterCaps; unsigned int ZCmpCaps; unsigned int SrcBlendCaps; unsigned int DestBlendCaps;
    unsigned int AlphaCmpCaps; unsigned int ShadeCaps; unsigned int TextureCaps; unsigned int TextureFilterCaps;
    unsigned int CubeTextureFilterCaps; unsigned int VolumeTextureFilterCaps; unsigned int TextureAddressCaps;
    unsigned int VolumeTextureAddressCaps; unsigned int LineCaps; unsigned int MaxTextureWidth; unsigned int MaxTextureHeight;
    unsigned int MaxVolumeExtent; unsigned int MaxTextureRepeat; unsigned int MaxTextureAspectRatio; unsigned int MaxAnisotropy;
    float MaxVertexW; float GuardBandLeft; float GuardBandTop; float GuardBandRight; float GuardBandBottom; float ExtentsAdjust;
    unsigned int StencilCaps; unsigned int FVFCaps; unsigned int TextureOpCaps; unsigned int MaxTextureBlendStages;
    unsigned int MaxSimultaneousTextures; unsigned int VertexProcessingCaps; unsigned int MaxActiveLights;
    unsigned int MaxUserClipPlanes; unsigned int MaxVertexBlendMatrices; unsigned int MaxVertexBlendMatrixIndex;
    float MaxPointSize; unsigned int MaxPrimitiveCount; unsigned int MaxVertexIndex; unsigned int MaxStreams;
    unsigned int MaxStreamStride; unsigned int VertexShaderVersion; unsigned int MaxVertexShaderConst;
    unsigned int PixelShaderVersion; float MaxPixelShaderValue;
} D3DCAPS8;

struct IDirect3D8; struct IDirect3DDevice8; struct IDirect3DTexture8; struct IDirect3DSurface8; struct IDirect3DVertexBuffer8;

typedef struct IDirect3DSurface8Vtbl {
    long (__stdcall *QueryInterface)(struct IDirect3DSurface8 *This, void *riid, void **ppv);
    unsigned int (__stdcall *AddRef)(struct IDirect3DSurface8 *This);
    unsigned int (__stdcall *Release)(struct IDirect3DSurface8 *This);
    long (__stdcall *GetDevice)(struct IDirect3DSurface8 *This, struct IDirect3DDevice8 **ppDevice);
    long (__stdcall *SetPrivateData)(struct IDirect3DSurface8 *This, void *refguid, void *pData, unsigned int SizeOfData, unsigned int Flags);
    long (__stdcall *GetPrivateData)(struct IDirect3DSurface8 *This, void *refguid, void *pData, unsigned int *pSizeOfData);
    long (__stdcall *FreePrivateData)(struct IDirect3DSurface8 *This, void *refguid);
    long (__stdcall *GetContainer)(struct IDirect3DSurface8 *This, void *riid, void **ppContainer);
    long (__stdcall *GetDesc)(struct IDirect3DSurface8 *This, D3DSURFACE_DESC *pDesc);
    long (__stdcall *LockRect)(struct IDirect3DSurface8 *This, D3DLOCKED_RECT *pLockedRect, void *pRect, unsigned int Flags);
    long (__stdcall *UnlockRect)(struct IDirect3DSurface8 *This);
} IDirect3DSurface8Vtbl;
typedef struct IDirect3DSurface8 { IDirect3DSurface8Vtbl *lpVtbl; } IDirect3DSurface8;

typedef struct IDirect3DTexture8Vtbl {
    long (__stdcall *QueryInterface)(struct IDirect3DTexture8 *This, void *riid, void **ppv);
    unsigned int (__stdcall *AddRef)(struct IDirect3DTexture8 *This);
    unsigned int (__stdcall *Release)(struct IDirect3DTexture8 *This);
    long (__stdcall *GetDevice)(struct IDirect3DTexture8 *This, struct IDirect3DDevice8 **ppDevice);
    long (__stdcall *SetPrivateData)(struct IDirect3DTexture8 *This, void *refguid, void *pData, unsigned int SizeOfData, unsigned int Flags);
    long (__stdcall *GetPrivateData)(struct IDirect3DTexture8 *This, void *refguid, void *pData, unsigned int *pSizeOfData);
    long (__stdcall *FreePrivateData)(struct IDirect3DTexture8 *This, void *refguid);
    unsigned int (__stdcall *SetPriority)(struct IDirect3DTexture8 *This, unsigned int PriorityNew);
    unsigned int (__stdcall *GetPriority)(struct IDirect3DTexture8 *This);
    void (__stdcall *PreLoad)(struct IDirect3DTexture8 *This);
    D3DRESOURCETYPE (__stdcall *GetType)(struct IDirect3DTexture8 *This);
    unsigned int (__stdcall *SetLOD)(struct IDirect3DTexture8 *This, unsigned int LODNew);
    unsigned int (__stdcall *GetLOD)(struct IDirect3DTexture8 *This);
    unsigned int (__stdcall *GetLevelCount)(struct IDirect3DTexture8 *This);
    long (__stdcall *GetLevelDesc)(struct IDirect3DTexture8 *This, unsigned int Level, D3DSURFACE_DESC *pDesc);
    long (__stdcall *GetSurfaceLevel)(struct IDirect3DTexture8 *This, unsigned int Level, struct IDirect3DSurface8 **ppSurfaceLevel);
    long (__stdcall *LockRect)(struct IDirect3DTexture8 *This, unsigned int Level, D3DLOCKED_RECT *pLockedRect, void *pRect, unsigned int Flags);
    long (__stdcall *UnlockRect)(struct IDirect3DTexture8 *This, unsigned int Level);
    long (__stdcall *AddDirtyRect)(struct IDirect3DTexture8 *This, void *pDirtyRect);
} IDirect3DTexture8Vtbl;
typedef struct IDirect3DTexture8 { IDirect3DTexture8Vtbl *lpVtbl; } IDirect3DTexture8;

typedef struct IDirect3DVertexBuffer8Vtbl {
    long (__stdcall *QueryInterface)(struct IDirect3DVertexBuffer8 *This, void *riid, void **ppv);
    unsigned int (__stdcall *AddRef)(struct IDirect3DVertexBuffer8 *This);
    unsigned int (__stdcall *Release)(struct IDirect3DVertexBuffer8 *This);
    long (__stdcall *GetDevice)(struct IDirect3DVertexBuffer8 *This, struct IDirect3DDevice8 **ppDevice);
    long (__stdcall *SetPrivateData)(struct IDirect3DVertexBuffer8 *This, void *refguid, void *pData, unsigned int SizeOfData, unsigned int Flags);
    long (__stdcall *GetPrivateData)(struct IDirect3DVertexBuffer8 *This, void *refguid, void *pData, unsigned int *pSizeOfData);
    long (__stdcall *FreePrivateData)(struct IDirect3DVertexBuffer8 *This, void *refguid);
    unsigned int (__stdcall *SetPriority)(struct IDirect3DVertexBuffer8 *This, unsigned int PriorityNew);
    unsigned int (__stdcall *GetPriority)(struct IDirect3DVertexBuffer8 *This);
    void (__stdcall *PreLoad)(struct IDirect3DVertexBuffer8 *This);
    D3DRESOURCETYPE (__stdcall *GetType)(struct IDirect3DVertexBuffer8 *This);
    long (__stdcall *Lock)(struct IDirect3DVertexBuffer8 *This, unsigned int OffsetToLock, unsigned int SizeToLock, unsigned char **ppbData, unsigned int Flags);
    long (__stdcall *Unlock)(struct IDirect3DVertexBuffer8 *This);
    long (__stdcall *GetDesc)(struct IDirect3DVertexBuffer8 *This, void *pDesc);
} IDirect3DVertexBuffer8Vtbl;
typedef struct IDirect3DVertexBuffer8 { IDirect3DVertexBuffer8Vtbl *lpVtbl; } IDirect3DVertexBuffer8;

typedef struct IDirect3D8Vtbl {
    long (__stdcall *QueryInterface)(struct IDirect3D8 *This, void *riid, void **ppv);
    unsigned int (__stdcall *AddRef)(struct IDirect3D8 *This);
    unsigned int (__stdcall *Release)(struct IDirect3D8 *This);
    long (__stdcall *RegisterSoftwareDevice)(struct IDirect3D8 *This, void *pInitializeFunction);
    unsigned int (__stdcall *GetAdapterCount)(struct IDirect3D8 *This);
    long (__stdcall *GetAdapterIdentifier)(struct IDirect3D8 *This, unsigned int Adapter, unsigned int Flags, void *pIdentifier);
    unsigned int (__stdcall *GetAdapterModeCount)(struct IDirect3D8 *This, unsigned int Adapter);
    long (__stdcall *EnumAdapterModes)(struct IDirect3D8 *This, unsigned int Adapter, unsigned int Mode, D3DDISPLAYMODE *pMode);
    long (__stdcall *GetAdapterDisplayMode)(struct IDirect3D8 *This, unsigned int Adapter, D3DDISPLAYMODE *pMode);
    long (__stdcall *CheckDeviceType)(struct IDirect3D8 *This, unsigned int Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, int Windowed);
    long (__stdcall *CheckDeviceFormat)(struct IDirect3D8 *This, unsigned int Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, unsigned int Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat);
    long (__stdcall *CheckDeviceMultiSampleType)(struct IDirect3D8 *This, unsigned int Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, int Windowed, D3DMULTISAMPLE_TYPE MultiSampleType);
    long (__stdcall *CheckDepthStencilMatch)(struct IDirect3D8 *This, unsigned int Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat);
    long (__stdcall *GetDeviceCaps)(struct IDirect3D8 *This, unsigned int Adapter, D3DDEVTYPE DeviceType, D3DCAPS8 *pCaps);
    void *(__stdcall *GetAdapterMonitor)(struct IDirect3D8 *This, unsigned int Adapter);
    long (__stdcall *CreateDevice)(struct IDirect3D8 *This, unsigned int Adapter, D3DDEVTYPE DeviceType, void *hFocusWindow, unsigned int BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, struct IDirect3DDevice8 **ppReturnedDeviceInterface);
} IDirect3D8Vtbl;
typedef struct IDirect3D8 { IDirect3D8Vtbl *lpVtbl; } IDirect3D8;

typedef struct IDirect3DDevice8Vtbl {
    long (__stdcall *QueryInterface)(struct IDirect3DDevice8 *This, void *riid, void **ppv);
    unsigned int (__stdcall *AddRef)(struct IDirect3DDevice8 *This);
    unsigned int (__stdcall *Release)(struct IDirect3DDevice8 *This);
    long (__stdcall *TestCooperativeLevel)(struct IDirect3DDevice8 *This);
    unsigned int (__stdcall *GetAvailableTextureMem)(struct IDirect3DDevice8 *This);
    long (__stdcall *ResourceManagerDiscardBytes)(struct IDirect3DDevice8 *This, unsigned int Bytes);
    long (__stdcall *GetDirect3D)(struct IDirect3DDevice8 *This, struct IDirect3D8 **ppD3D8);
    long (__stdcall *GetDeviceCaps)(struct IDirect3DDevice8 *This, D3DCAPS8 *pCaps);
    long (__stdcall *GetDisplayMode)(struct IDirect3DDevice8 *This, D3DDISPLAYMODE *pMode);
    long (__stdcall *GetCreationParameters)(struct IDirect3DDevice8 *This, void *pParameters);
    long (__stdcall *SetCursorProperties)(struct IDirect3DDevice8 *This, unsigned int XHotSpot, unsigned int YHotSpot, struct IDirect3DSurface8 *pCursorBitmap);
    void (__stdcall *SetCursorPosition)(struct IDirect3DDevice8 *This, int X, int Y, unsigned int Flags);
    int (__stdcall *ShowCursor)(struct IDirect3DDevice8 *This, int bShow);
    long (__stdcall *CreateAdditionalSwapChain)(struct IDirect3DDevice8 *This, D3DPRESENT_PARAMETERS *pPresentationParameters, void **pSwapChain);
    long (__stdcall *Reset)(struct IDirect3DDevice8 *This, D3DPRESENT_PARAMETERS *pPresentationParameters);
    long (__stdcall *Present)(struct IDirect3DDevice8 *This, void *pSourceRect, void *pDestRect, void *hDestWindowOverride, void *pDirtyRegion);
    long (__stdcall *GetBackBuffer)(struct IDirect3DDevice8 *This, unsigned int BackBuffer, D3DBACKBUFFER_TYPE Type, struct IDirect3DSurface8 **ppBackBuffer);
    long (__stdcall *GetRasterStatus)(struct IDirect3DDevice8 *This, void *pRasterStatus);
    void (__stdcall *SetGammaRamp)(struct IDirect3DDevice8 *This, unsigned int Flags, D3DGAMMARAMP *pRamp);
    void (__stdcall *GetGammaRamp)(struct IDirect3DDevice8 *This, D3DGAMMARAMP *pRamp);
    long (__stdcall *CreateTexture)(struct IDirect3DDevice8 *This, unsigned int Width, unsigned int Height, unsigned int Levels, unsigned int Usage, D3DFORMAT Format, D3DPOOL Pool, struct IDirect3DTexture8 **ppTexture);
    long (__stdcall *CreateVolumeTexture)(struct IDirect3DDevice8 *This, unsigned int Width, unsigned int Height, unsigned int Depth, unsigned int Levels, unsigned int Usage, D3DFORMAT Format, D3DPOOL Pool, void **ppVolumeTexture);
    long (__stdcall *CreateCubeTexture)(struct IDirect3DDevice8 *This, unsigned int EdgeLength, unsigned int Levels, unsigned int Usage, D3DFORMAT Format, D3DPOOL Pool, void **ppCubeTexture);
    long (__stdcall *CreateVertexBuffer)(struct IDirect3DDevice8 *This, unsigned int Length, unsigned int Usage, unsigned int FVF, D3DPOOL Pool, struct IDirect3DVertexBuffer8 **ppVertexBuffer);
    long (__stdcall *CreateIndexBuffer)(struct IDirect3DDevice8 *This, unsigned int Length, unsigned int Usage, D3DFORMAT Format, D3DPOOL Pool, void **ppIndexBuffer);
    long (__stdcall *CreateRenderTarget)(struct IDirect3DDevice8 *This, unsigned int Width, unsigned int Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, int Lockable, struct IDirect3DSurface8 **ppSurface);
    long (__stdcall *CreateDepthStencilSurface)(struct IDirect3DDevice8 *This, unsigned int Width, unsigned int Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, struct IDirect3DSurface8 **ppSurface);
    long (__stdcall *CreateImageSurface)(struct IDirect3DDevice8 *This, unsigned int Width, unsigned int Height, D3DFORMAT Format, struct IDirect3DSurface8 **ppSurface);
    long (__stdcall *CopyRects)(struct IDirect3DDevice8 *This, struct IDirect3DSurface8 *pSourceSurface, void *pSourceRectsArray, unsigned int cRects, struct IDirect3DSurface8 *pDestinationSurface, void *pDestPointsArray);
    long (__stdcall *UpdateTexture)(struct IDirect3DDevice8 *This, void *pSourceTexture, void *pDestinationTexture);
    long (__stdcall *GetFrontBuffer)(struct IDirect3DDevice8 *This, struct IDirect3DSurface8 *pDestSurface);
    long (__stdcall *SetRenderTarget)(struct IDirect3DDevice8 *This, struct IDirect3DSurface8 *pRenderTarget, struct IDirect3DSurface8 *pNewZStencil);
    long (__stdcall *GetRenderTarget)(struct IDirect3DDevice8 *This, struct IDirect3DSurface8 **ppRenderTarget);
    long (__stdcall *GetDepthStencilSurface)(struct IDirect3DDevice8 *This, struct IDirect3DSurface8 **ppZStencilSurface);
    long (__stdcall *BeginScene)(struct IDirect3DDevice8 *This);
    long (__stdcall *EndScene)(struct IDirect3DDevice8 *This);
    long (__stdcall *Clear)(struct IDirect3DDevice8 *This, unsigned int Count, D3DRECT *pRects, unsigned int Flags, unsigned int Color, float Z, unsigned int Stencil);
    long (__stdcall *SetTransform)(struct IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix);
    long (__stdcall *GetTransform)(struct IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix);
    long (__stdcall *MultiplyTransform)(struct IDirect3DDevice8 *This, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix);
    long (__stdcall *SetViewport)(struct IDirect3DDevice8 *This, D3DVIEWPORT8 *pViewport);
    long (__stdcall *GetViewport)(struct IDirect3DDevice8 *This, D3DVIEWPORT8 *pViewport);
    long (__stdcall *SetMaterial)(struct IDirect3DDevice8 *This, D3DMATERIAL8 *pMaterial);
    long (__stdcall *GetMaterial)(struct IDirect3DDevice8 *This, D3DMATERIAL8 *pMaterial);
    long (__stdcall *SetLight)(struct IDirect3DDevice8 *This, unsigned int Index, D3DLIGHT8 *pLight);
    long (__stdcall *GetLight)(struct IDirect3DDevice8 *This, unsigned int Index, D3DLIGHT8 *pLight);
    long (__stdcall *LightEnable)(struct IDirect3DDevice8 *This, unsigned int Index, int Enable);
    long (__stdcall *GetLightEnable)(struct IDirect3DDevice8 *This, unsigned int Index, int *pEnable);
    long (__stdcall *SetClipPlane)(struct IDirect3DDevice8 *This, unsigned int Index, float *pPlane);
    long (__stdcall *GetClipPlane)(struct IDirect3DDevice8 *This, unsigned int Index, float *pPlane);
    long (__stdcall *SetRenderState)(struct IDirect3DDevice8 *This, D3DRENDERSTATETYPE State, unsigned int Value);
    long (__stdcall *GetRenderState)(struct IDirect3DDevice8 *This, D3DRENDERSTATETYPE State, unsigned int *pValue);
    long (__stdcall *BeginStateBlock)(struct IDirect3DDevice8 *This);
    long (__stdcall *EndStateBlock)(struct IDirect3DDevice8 *This, unsigned int *pToken);
    long (__stdcall *ApplyStateBlock)(struct IDirect3DDevice8 *This, unsigned int Token);
    long (__stdcall *CaptureStateBlock)(struct IDirect3DDevice8 *This, unsigned int Token);
    long (__stdcall *DeleteStateBlock)(struct IDirect3DDevice8 *This, unsigned int Token);
    long (__stdcall *CreateStateBlock)(struct IDirect3DDevice8 *This, unsigned int Type, unsigned int *pToken);
    long (__stdcall *SetClipStatus)(struct IDirect3DDevice8 *This, void *pClipStatus);
    long (__stdcall *GetClipStatus)(struct IDirect3DDevice8 *This, void *pClipStatus);
    long (__stdcall *GetTexture)(struct IDirect3DDevice8 *This, unsigned int Stage, struct IDirect3DTexture8 **ppTexture);
    long (__stdcall *SetTexture)(struct IDirect3DDevice8 *This, unsigned int Stage, struct IDirect3DTexture8 *pTexture);
    long (__stdcall *GetTextureStageState)(struct IDirect3DDevice8 *This, unsigned int Stage, D3DTEXTURESTAGESTATETYPE Type, unsigned int *pValue);
    long (__stdcall *SetTextureStageState)(struct IDirect3DDevice8 *This, unsigned int Stage, D3DTEXTURESTAGESTATETYPE Type, unsigned int Value);
    long (__stdcall *ValidateDevice)(struct IDirect3DDevice8 *This, unsigned int *pNumPasses);
    long (__stdcall *GetInfo)(struct IDirect3DDevice8 *This, unsigned int DevInfoID, void *pDevInfoStruct, unsigned int DevInfoStructSize);
    long (__stdcall *SetPaletteEntries)(struct IDirect3DDevice8 *This, unsigned int PaletteNumber, void *pEntries);
    long (__stdcall *GetPaletteEntries)(struct IDirect3DDevice8 *This, unsigned int PaletteNumber, void *pEntries);
    long (__stdcall *SetCurrentTexturePalette)(struct IDirect3DDevice8 *This, unsigned int PaletteNumber);
    long (__stdcall *GetCurrentTexturePalette)(struct IDirect3DDevice8 *This, unsigned int *PaletteNumber);
    long (__stdcall *DrawPrimitive)(struct IDirect3DDevice8 *This, D3DPRIMITIVETYPE PrimitiveType, unsigned int StartVertex, unsigned int PrimitiveCount);
    long (__stdcall *DrawIndexedPrimitive)(struct IDirect3DDevice8 *This, D3DPRIMITIVETYPE PrimitiveType, unsigned int minIndex, unsigned int NumVertices, unsigned int startIndex, unsigned int primCount);
    long (__stdcall *DrawPrimitiveUP)(struct IDirect3DDevice8 *This, D3DPRIMITIVETYPE PrimitiveType, unsigned int PrimitiveCount, void *pVertexStreamZeroData, unsigned int VertexStreamZeroStride);
    long (__stdcall *DrawIndexedPrimitiveUP)(struct IDirect3DDevice8 *This, D3DPRIMITIVETYPE PrimitiveType, unsigned int MinVertexIndex, unsigned int NumVertexIndices, unsigned int PrimitiveCount, void *pIndexData, D3DFORMAT IndexDataFormat, void *pVertexStreamZeroData, unsigned int VertexStreamZeroStride);
    long (__stdcall *ProcessVertices)(struct IDirect3DDevice8 *This, unsigned int SrcStartIndex, unsigned int DestIndex, unsigned int VertexCount, struct IDirect3DVertexBuffer8 *pDestBuffer, unsigned int Flags);
    long (__stdcall *CreateVertexShader)(struct IDirect3DDevice8 *This, unsigned int *pDeclaration, unsigned int *pFunction, unsigned int *pHandle, unsigned int Usage);
    long (__stdcall *SetVertexShader)(struct IDirect3DDevice8 *This, unsigned int Handle);
    long (__stdcall *GetVertexShader)(struct IDirect3DDevice8 *This, unsigned int *pHandle);
    long (__stdcall *DeleteVertexShader)(struct IDirect3DDevice8 *This, unsigned int Handle);
    long (__stdcall *SetVertexShaderConstant)(struct IDirect3DDevice8 *This, unsigned int Register, void *pConstantData, unsigned int ConstantCount);
    long (__stdcall *GetVertexShaderConstant)(struct IDirect3DDevice8 *This, unsigned int Register, void *pConstantData, unsigned int ConstantCount);
    long (__stdcall *GetVertexShaderDeclaration)(struct IDirect3DDevice8 *This, unsigned int Handle, void *pData, unsigned int *pSizeOfData);
    long (__stdcall *GetVertexShaderFunction)(struct IDirect3DDevice8 *This, unsigned int Handle, void *pData, unsigned int *pSizeOfData);
    long (__stdcall *SetStreamSource)(struct IDirect3DDevice8 *This, unsigned int StreamNumber, struct IDirect3DVertexBuffer8 *pStreamData, unsigned int Stride);
    long (__stdcall *GetStreamSource)(struct IDirect3DDevice8 *This, unsigned int StreamNumber, struct IDirect3DVertexBuffer8 **ppStreamData, unsigned int *pStride);
    long (__stdcall *SetIndices)(struct IDirect3DDevice8 *This, void *pIndexData, unsigned int BaseVertexIndex);
    long (__stdcall *GetIndices)(struct IDirect3DDevice8 *This, void **ppIndexData, unsigned int *pBaseVertexIndex);
    long (__stdcall *CreatePixelShader)(struct IDirect3DDevice8 *This, unsigned int *pFunction, unsigned int *pHandle);
    long (__stdcall *SetPixelShader)(struct IDirect3DDevice8 *This, unsigned int Handle);
    long (__stdcall *GetPixelShader)(struct IDirect3DDevice8 *This, unsigned int *pHandle);
    long (__stdcall *DeletePixelShader)(struct IDirect3DDevice8 *This, unsigned int Handle);
    long (__stdcall *SetPixelShaderConstant)(struct IDirect3DDevice8 *This, unsigned int Register, void *pConstantData, unsigned int ConstantCount);
    long (__stdcall *GetPixelShaderConstant)(struct IDirect3DDevice8 *This, unsigned int Register, void *pConstantData, unsigned int ConstantCount);
    long (__stdcall *GetPixelShaderFunction)(struct IDirect3DDevice8 *This, unsigned int Handle, void *pData, unsigned int *pSizeOfData);
    long (__stdcall *DrawRectPatch)(struct IDirect3DDevice8 *This, unsigned int Handle, float *pNumSegs, void *pRectPatchInfo);
    long (__stdcall *DrawTriPatch)(struct IDirect3DDevice8 *This, unsigned int Handle, float *pNumSegs, void *pTriPatchInfo);
    long (__stdcall *DeletePatch)(struct IDirect3DDevice8 *This, unsigned int Handle);
} IDirect3DDevice8Vtbl;
typedef struct IDirect3DDevice8 { IDirect3DDevice8Vtbl *lpVtbl; } IDirect3DDevice8;
