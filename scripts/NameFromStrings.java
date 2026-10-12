// Rename unnamed functions after the "Class::Method" names found in the log strings they reference.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.StringDataInstance;
import java.util.*;
import java.util.regex.*;

public class NameFromStrings extends GhidraScript {
    @Override
    public void run() throws Exception {
        Pattern p = Pattern.compile("^(C[A-Za-z0-9_]+)::([A-Za-z_][A-Za-z0-9_]*)\\s*(\\(\\)|:|\\()");
        Map<Function, Set<String>> cand = new HashMap<>();
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        for (Data d : listing.getDefinedData(true)) {
            if (!d.hasStringValue()) continue;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s == null) continue;
            Matcher m = p.matcher(s);
            if (!m.find()) continue;
            String member = m.group(2);
            // "CGraphic::m_pD3DD->..." names a member variable, not the calling method
            if (member.startsWith("m_")) continue;
            String name = m.group(1) + "::" + member;
            for (Reference r : rm.getReferencesTo(d.getAddress())) {
                Function f = fm.getFunctionContaining(r.getFromAddress());
                if (f != null) cand.computeIfAbsent(f, k -> new TreeSet<>()).add(name);
            }
        }
        int renamed = 0, ambiguous = 0;
        for (Map.Entry<Function, Set<String>> e : cand.entrySet()) {
            Function f = e.getKey();
            if (!f.getName().startsWith("FUN_")) continue;
            if (e.getValue().size() != 1) { ambiguous++; println("ambiguous " + f.getEntryPoint() + " " + e.getValue()); continue; }
            String[] parts = e.getValue().iterator().next().split("::");
            Namespace ns = currentProgram.getSymbolTable().getNamespace(parts[0], null);
            if (ns == null) ns = currentProgram.getSymbolTable().createClass(null, parts[0], SourceType.ANALYSIS);
            f.setParentNamespace(ns);
            f.setName(parts[1], SourceType.ANALYSIS);
            renamed++;
        }
        println("RENAMED " + renamed + " ambiguous=" + ambiguous);
    }
}
