// Decompile functions in [start, end) to one file. Args: <outFile> <startHex> <endHex>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        long start = Long.parseLong(a[1], 16), end = Long.parseLong(a[2], 16);
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        int n = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(start), true)) {
            if (f.getEntryPoint().getOffset() >= end) break;
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            out.println("// ==== " + f.getEntryPoint() + " " + f.getName(true) + " ====");
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            n++;
        }
        out.close();
        println("EXPORTED " + n);
    }
}
