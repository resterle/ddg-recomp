// Apply D3D8 types, CGraphic globals, function names and signatures to perfect.exe.
// Args: <include dir>
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ApplyCGraphic extends GhidraScript {
    DataTypeManager dtm;
    CParser parser;

    DataType type(String name) {
        DataType t = parser.getTypes().get(name);
        if (t == null) t = parser.getComposites().get(name);
        if (t == null) throw new RuntimeException("type not found: " + name);
        return dtm.resolve(t, DataTypeConflictHandler.REPLACE_HANDLER);
    }

    Namespace ns(String name) throws Exception {
        Namespace n = currentProgram.getSymbolTable().getNamespace(name, null);
        return n != null ? n : currentProgram.getSymbolTable().createClass(null, name, SourceType.USER_DEFINED);
    }

    void global(long addr, String name, DataType t) throws Exception {
        Address a = toAddr(addr);
        clearListing(a, a.add(t.getLength() - 1));
        createData(a, t);
        Symbol old = getSymbolAt(a);
        if (old != null && old.getSource() == SourceType.DEFAULT) { /* replaced below */ }
        createLabel(a, name, ns("CGraphic"), true, SourceType.USER_DEFINED);
    }

    void fn(long addr, String cls, String name, String proto) throws Exception {
        Function f = getFunctionAt(toAddr(addr));
        if (f == null) f = createFunction(toAddr(addr), null);
        if (proto != null) {
            DataType fd = parser.getFunctions().get(proto);
            if (fd == null) throw new RuntimeException("prototype not found: " + proto);
            new ApplyFunctionSignatureCmd(f.getEntryPoint(), (FunctionSignature) fd, SourceType.USER_DEFINED)
                .applyTo(currentProgram, monitor);
            if (((FunctionSignature) fd).getCallingConventionName().equals("unknown")
                    || ((FunctionSignature) fd).getCallingConventionName().equals("default"))
                f.setCallingConvention("__cdecl");
        }
        if (cls != null) f.setParentNamespace(ns(cls));
        f.setName(name, SourceType.USER_DEFINED);
    }

    @Override
    public void run() throws Exception {
        String inc = getScriptArgs()[0];
        dtm = currentProgram.getDataTypeManager();
        parser = new CParser(dtm, true, null);
        StringBuilder src = new StringBuilder();
        for (String f : new String[] {"d3d8_min.h", "graphic_types.h"}) {
            src.append(new String(java.nio.file.Files.readAllBytes(new File(inc, f).toPath()))).append('\n');
        }
        parser.parse(src.toString());

        DataType u32 = UnsignedIntegerDataType.dataType, i32 = IntegerDataType.dataType, f32 = FloatDataType.dataType;
        DataType mat = type("D3DMATRIX");

        // CGraphic static members
        global(0x4c71e0, "m_nPrimType", type("D3DPRIMITIVETYPE"));
        global(0x4c71e4, "m_nTexID", u32);
        global(0x4c71f0, "m_TexSize", type("TEXSIZE"));
        global(0x4c71f8, "m_fGamma", f32);
        global(0x4c71fc, "m_nD3DFTexture", type("D3DFORMAT"));
        global(0x4c7200, "m_nD3DTOPModulate", u32);
        global(0x4c7204, "m_nMaxTexWidth", u32);
        global(0x4c7208, "m_nMaxTexHeight", u32);
        global(0x4c7210, "m_Config", type("GRAPHIC_CONFIG"));
        global(0x5eb748, "m_DisplayMode", type("D3DDISPLAYMODE"));
        global(0x5eb758, "m_D3DPP", type("D3DPRESENT_PARAMETERS"));
        global(0x5eb790, "m_csDraw", type("CRITICAL_SECTION_"));
        global(0x5eb7a8, "m_TexVertex", new ArrayDataType(type("TLVERTEX_TEX"), 4, 28));
        global(0x5eb818, "m_Vertex2D", new ArrayDataType(type("TLVERTEX"), 4, 20));
        global(0x5eb868, "m_D3DCaps", type("D3DCAPS8"));
        global(0x6105c0, "m_Light", type("D3DLIGHT8"));
        global(0x610628, "m_Material", type("D3DMATERIAL8"));
        global(0x610670, "m_matProj", mat);
        global(0x6106b0, "m_matView", mat);
        global(0x6106f0, "m_matWorld", mat);
        global(0x610730, "m_TexInfo", new ArrayDataType(type("TEXINFO"), 0x4000, 24));
        global(0x670734, "m_pD3D", new PointerDataType(type("IDirect3D8")));
        global(0x670738, "m_pD3DD", new PointerDataType(type("IDirect3DDevice8")));
        global(0x670740, "m_pD3DVBRT", new PointerDataType(type("IDirect3DVertexBuffer8")));
        global(0x670748, "m_pTexInf", new PointerDataType(type("TEXINFO")));
        global(0x67074c, "m_pPalette", new PointerDataType(u32));
        global(0x670750, "m_bScaleScreen", i32);
        global(0x670754, "m_nViewport", i32);
        global(0x670768, "m_rcBorder", new ArrayDataType(type("D3DRECT"), 4, 16));
        global(0x6707a8, "m_Viewport", new ArrayDataType(type("D3DVIEWPORT8"), 2, 24));
        global(0x672ecc, "m_szResetError", new ArrayDataType(CharDataType.dataType, 128, 1));
        global(0x672f4c, "m_szLastError", new ArrayDataType(CharDataType.dataType, 128, 1));
        global(0x672fcc, "m_hr", i32);
        global(0x672fd0, "m_hrReset", i32);

        // CGraphic methods (Graphic.cpp, 0x402110-0x4055a0)
        fn(0x402110, "CGraphic", "LoadConfig", "CGraphic_LoadConfig");
        fn(0x402350, "CGraphic", "Init", "CGraphic_Init");
        fn(0x402a50, "CGraphic", "Term", "CGraphic_Term");
        fn(0x402ae0, "CGraphic", "Reset", "CGraphic_Reset");
        fn(0x402e50, "CGraphic", "GetAdapterFormat", "CGraphic_GetAdapterFormat");
        fn(0x402f20, "CGraphic", "GetDepthStencilFormat", "CGraphic_GetDepthStencilFormat");
        fn(0x403170, "CGraphic", "GetTextureFormat", "CGraphic_GetTextureFormat");
        fn(0x403240, "CGraphic", "GetRefreshRate", "CGraphic_GetRefreshRate");
        fn(0x403330, "CGraphic", "GetTextureModulate", "CGraphic_GetTextureModulate");
        fn(0x4033f0, "CGraphic", "GetMaxTextureWidth", "CGraphic_GetMaxTextureWidth");
        fn(0x403480, "CGraphic", "GetMaxTextureHeight", "CGraphic_GetMaxTextureHeight");
        fn(0x403510, "CGraphic", "GetBehaviorFlags", "CGraphic_GetBehaviorFlags");
        fn(0x403560, "CGraphic", "SetGamma", "CGraphic_SetGamma");
        fn(0x403630, "CGraphic", "FindTexSize", "CGraphic_FindTexSize");
        fn(0x403660, "CGraphic", "LoadTexture", null);
        fn(0x403840, "CGraphic", "SetPalette", null);
        fn(0x403900, "CGraphic", "SetTexture", "CGraphic_SetTexture");
        fn(0x403a60, "CGraphic", "NormalizeUV", "CGraphic_NormalizeUV");
        fn(0x403ac0, "CGraphic", "SetVertex", "CGraphic_SetVertex");
        fn(0x403b70, "CGraphic", "AddStripVertex", "CGraphic_AddStripVertex");
        fn(0x403c80, "CGraphic", "SetSpriteRect", "CGraphic_SetSpriteRect");
        fn(0x403e10, "CGraphic", "DrawPrimitive", null);
        fn(0x404110, "CGraphic", "ClearBorders", "CGraphic_ClearBorders");
        fn(0x404390, "CGraphic", "SetViewport", "CGraphic_SetViewport");
        fn(0x404630, "CGraphic", "PrintScreen", "CGraphic_PrintScreen");
        fn(0x404a60, "CGraphic", "CreateRenderTargets", "CGraphic_CreateRenderTargets");
        fn(0x404e50, "CGraphic", "ReleaseRenderTargets", "CGraphic_ReleaseRenderTargets");
        fn(0x404e90, "CGraphic", "SetRenderTargetTexture", "CGraphic_SetRenderTargetTexture");
        fn(0x404ff0, "CGraphic", "ApplyScreenEffect", "CGraphic_ApplyScreenEffect");

        // RAII critical-section lock (vtable at 0x4b2378)
        fn(0x4040c0, "CLock", "~CLock", null);
        fn(0x4040e0, "CLock", "scalar_deleting_destructor", null);

        // Members of other unb classes called from CGraphic (names from "m_Vertex.Reset()" etc.)
        fn(0x411300, "CVertex", "Init", null);
        fn(0x411370, "CVertex", "Term", null);
        fn(0x4114a0, "CVertex", "Reset", null);
        fn(0x40a370, "CTexture", "Init", null);
        fn(0x40a700, "CTexture", "Term", null);
        fn(0x40a7a0, "CTexture", "Reset", null);

        // Library and helper routines
        fn(0x40d0b0, null, "DebugLog", "DebugLog");
        fn(0x40d030, null, "FormatString", "FormatString");
        fn(0x401000, null, "RegQueryDword", null);
        fn(0x401040, null, "RegQueryBinary", null);
        fn(0x401cc0, null, "D3DFormatToString", "D3DFormatToString");
        fn(0x484126, null, "DXGetErrorString8A", "DXGetErrorString8A");
        fn(0x486ab4, null, "DXGetErrorDescription8A", "DXGetErrorDescription8A");
        fn(0x48d049, null, "D3DXCheckTextureRequirements", "D3DXCheckTextureRequirements");
        fn(0x48ae21, null, "D3DXMatrixLookAtLH", "D3DXMatrixLookAtLH");
        fn(0x48af69, null, "D3DXMatrixPerspectiveFovLH", "D3DXMatrixPerspectiveFovLH");
        fn(0x4a2ce2, null, "operator_delete", null);

        // Import thunk for d3d8.dll!Direct3DCreate8 had no signature, which broke stack tracking in Init.
        Function d3dc = getFunctionAt(toAddr(0x49adf8));
        Function ext = d3dc.isThunk() ? d3dc.getThunkedFunction(true) : d3dc;
        new ApplyFunctionSignatureCmd(ext.getEntryPoint(), (FunctionSignature) parser.getFunctions().get("Direct3DCreate8"),
            SourceType.USER_DEFINED).applyTo(currentProgram, monitor);

        println("APPLIED CGraphic");
    }
}
