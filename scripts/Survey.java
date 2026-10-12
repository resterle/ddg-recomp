// Per-function facts and function-pointer tables, for sizing the port.
// Args: <out dir>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Survey extends GhidraScript {
    @Override
    public void run() throws Exception {
        String dir = getScriptArgs()[0];
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();

        PrintWriter fo = new PrintWriter(new FileWriter(dir + "/funcs.tsv"));
        fo.println("entry\tsize\tinstr\tfpu\tsse\tcalls\ticalls\tijmps\tcallers\tcallees\tdatarefs_in\tcallee_list\tcaller_list");
        for (Function f : fm.getFunctions(true)) {
            if (f.isExternal() || f.isThunk()) continue;
            int n = 0, fpu = 0, sse = 0, calls = 0, icalls = 0, ijmps = 0;
            for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                n++;
                String m = ins.getMnemonicString().toUpperCase();
                if (m.startsWith("F")) fpu++;
                if (m.endsWith("PS") || m.endsWith("SS") || m.startsWith("MOVQ") || m.startsWith("PUNPCK")) sse++;
                if (ins.getFlowType().isCall()) {
                    calls++;
                    if (ins.getFlowType().isComputed()) icalls++;
                }
                if (ins.getFlowType().isJump() && ins.getFlowType().isComputed()) ijmps++;
            }
            StringBuilder cl = new StringBuilder(), cr = new StringBuilder();
            Set<Function> callees = f.getCalledFunctions(monitor), callers = f.getCallingFunctions(monitor);
            for (Function c : callees) cl.append(c.getEntryPoint()).append(',');
            for (Function c : callers) cr.append(c.getEntryPoint()).append(',');
            int dataIn = 0;
            for (Reference r : currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint()))
                if (r.getReferenceType().isData()) dataIn++;
            fo.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + n + "\t" + fpu + "\t" + sse + "\t" +
                       calls + "\t" + icalls + "\t" + ijmps + "\t" + callers.size() + "\t" + callees.size() + "\t" + dataIn +
                       "\t" + cl + "\t" + cr);
        }
        fo.close();

        // Runs of >= 2 consecutive dwords in initialised data that point at function entries.
        PrintWriter to = new PrintWriter(new FileWriter(dir + "/tables.tsv"));
        to.println("table\tcount\ttargets");
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.isExecute() || !b.isInitialized()) continue;
            Address a = b.getStart();
            List<String> run = new ArrayList<>();
            Address runStart = null;
            while (a.compareTo(b.getEnd()) < 0 && a.add(3).compareTo(b.getEnd()) <= 0) {
                int v = currentProgram.getMemory().getInt(a);
                Address t = toAddr(v & 0xffffffffL);
                Function f = (v != 0) ? fm.getFunctionAt(t) : null;
                if (f != null && !f.isExternal()) {
                    if (runStart == null) runStart = a;
                    run.add(t.toString());
                } else if (runStart != null) {
                    if (run.size() >= 2) to.println(runStart + "\t" + run.size() + "\t" + String.join(",", run));
                    runStart = null; run.clear();
                }
                a = a.add(4);
            }
        }
        to.close();
        println("SURVEY done");
    }
}
