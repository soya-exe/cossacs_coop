// Lists every imported function of a library (default WS2_32.DLL): its IAT slot, the jmp-thunks
// that go through it, and every call site (direct "call [IAT]" or a call to a thunk), deduplicated,
// with the containing function and its callers (two levels up).
// Args: <out.txt> [LIB]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.*;

import java.io.PrintWriter;
import java.util.*;

public class WsXrefs extends GhidraScript {
    String callers(Function f, int depth) {
        if (f == null || depth == 0) return "";
        StringBuilder sb = new StringBuilder();
        for (Function c : f.getCallingFunctions(monitor)) {
            sb.append(c.getName().replace("FUN_", ""));
            if (depth > 1) {
                String up = callers(c, depth - 1);
                if (!up.isEmpty()) sb.append("<{").append(up).append("}");
            }
            sb.append(' ');
        }
        return sb.toString().trim();
    }

    // collect call sites reaching target (an IAT slot or a thunk entry)
    void collect(Address target, boolean isIat, TreeMap<Address, String> sites, List<String> thunks,
                 Set<Address> seen) {
        for (Reference r : getReferencesTo(target)) {
            Address from = r.getFromAddress();
            Instruction ins = getInstructionAt(from);
            Function f = getFunctionContaining(from);
            if (ins == null) continue; // data ref (e.g. the import table itself)
            String m = ins.getMnemonicString();
            if (m.startsWith("JMP")) { // thunk "jmp [IAT]" or "jmp thunk"
                Address entry = f != null ? f.getEntryPoint() : from;
                if (seen.add(entry)) {
                    thunks.add(entry.toString());
                    collect(entry, false, sites, thunks, seen);
                    if (!entry.equals(from)) collect(from, false, sites, thunks, seen);
                }
            } else if (m.startsWith("CALL")) {
                sites.put(from, "call");
            } else { // e.g. MOV reg,[IAT] then CALL reg
                sites.put(from, "load:" + ins.toString());
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String lib = args.length > 1 ? args[1] : "WS2_32.DLL";
        PrintWriter w = new PrintWriter(args[0]);
        ExternalManager em = currentProgram.getExternalManager();
        ExternalLocationIterator it = em.getExternalLocations(lib);
        while (it.hasNext()) {
            ExternalLocation loc = it.next();
            Symbol s = loc.getSymbol();
            w.println("=================================================================");
            w.println("## " + lib + " " + loc.getLabel());
            TreeMap<Address, String> sites = new TreeMap<>();
            List<String> thunks = new ArrayList<>();
            Set<Address> seen = new HashSet<>();
            for (Reference r : s.getReferences()) {
                Address from = r.getFromAddress();
                if (getInstructionAt(from) == null) {
                    w.println("IAT slot " + from);
                    collect(from, true, sites, thunks, seen);
                }
            }
            w.println("thunks: " + thunks);
            for (Map.Entry<Address, String> e : sites.entrySet()) {
                Function f = getFunctionContaining(e.getKey());
                w.println("  " + e.getKey() + " " + e.getValue() + " in "
                    + (f == null ? "?" : f.getName().replace("FUN_", "")) + "   <- " + callers(f, 2));
            }
        }
        w.close();
    }
}
