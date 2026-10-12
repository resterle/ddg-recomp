// Pin signatures on indirect COM calls (CALL [reg+off]) so the decompiler knows each callee's
// stack purge. Ghidra resolves stack depth before it propagates struct types, so typed vtable
// pointers alone don't fix these call sites. Targets come from reading the disassembly.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import java.util.*;

public class ApplyCallOverrides extends GhidraScript {
    FunctionSignature method(String vtbl, String name) {
        List<DataType> found = new ArrayList<>();
        currentProgram.getDataTypeManager().findDataTypes(vtbl, found);
        for (DataType t : found) {
            if (!(t instanceof Structure)) continue;
            for (DataTypeComponent c : ((Structure) t).getComponents()) {
                if (name.equals(c.getFieldName()) && c.getDataType() instanceof Pointer) {
                    return (FunctionSignature) ((Pointer) c.getDataType()).getDataType();
                }
            }
        }
        throw new RuntimeException("method not found: " + vtbl + "." + name);
    }

    @Override
    public void run() throws Exception {
        String D3D = "IDirect3D8Vtbl", DEV = "IDirect3DDevice8Vtbl", TEX = "IDirect3DTexture8Vtbl", SURF = "IDirect3DSurface8Vtbl";
        Object[][] sites = {
            // CGraphic::Init
            {0x4023abL, D3D, "GetDeviceCaps"}, {0x4024b9L, D3D, "GetAdapterDisplayMode"},
            {0x402631L, D3D, "CreateDevice"}, {0x402677L, D3D, "CreateDevice"},
            {0x40272fL, DEV, "ShowCursor"}, {0x402772L, DEV, "SetRenderState"}, {0x4027e5L, DEV, "SetRenderState"},
            {0x4029b4L, DEV, "Clear"},
            // CGraphic::GetRefreshRate
            {0x403254L, D3D, "GetAdapterModeCount"}, {0x403271L, D3D, "EnumAdapterModes"},
            // CGraphic::SetTexture
            {0x40393fL, DEV, "SetTexture"}, {0x4039f5L, DEV, "SetCurrentTexturePalette"},
            // CGraphic::PrintScreen
            {0x404648L, DEV, "GetBackBuffer"}, {0x4046aaL, SURF, "GetDesc"}, {0x40473bL, SURF, "LockRect"},
            {0x404851L, SURF, "UnlockRect"}, {0x4048b2L, SURF, "Release"}, {0x404a17L, SURF, "UnlockRect"},
            {0x404a51L, SURF, "Release"},
            // CGraphic::SetRenderTargetTexture
            {0x404ea9L, TEX, "GetSurfaceLevel"}, {0x404f06L, DEV, "GetDepthStencilSurface"},
            {0x404f6bL, DEV, "SetRenderTarget"}, {0x404fc6L, SURF, "Release"}, {0x404fdcL, SURF, "Release"},
        };
        int n = 0;
        for (Object[] s : sites) {
            ghidra.program.model.address.Address at = toAddr((Long) s[0]);
            Instruction ins = getInstructionAt(at);
            if (ins == null || !ins.getFlowType().isCall()) throw new RuntimeException("not a call: " + at);
            Function f = getFunctionContaining(at);
            HighFunctionDBUtil.writeOverride(f, at, method((String) s[1], (String) s[2]));
            n++;
        }
        println("OVERRIDES " + n);
    }
}
