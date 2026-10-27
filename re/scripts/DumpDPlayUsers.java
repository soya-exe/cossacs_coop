// Finds code that touches DirectPlay: references to DirectPlay GUIDs in .rdata and calls to
// CoCreateInstance / DPLAYX imports, then decompiles every such function into one text file.
// Usage: analyzeHeadless <proj> btw -process dmcr.exe -noanalysis -scriptPath re/scripts
//        -postScript DumpDPlayUsers.java <out.txt>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

import java.io.PrintWriter;
import java.util.*;

public class DumpDPlayUsers extends GhidraScript {
    // name -> GUID in little-endian byte order as stored in memory
    static final String[][] GUIDS = {
        {"CLSID_DirectPlay", "d1eb6d20-8923-11d0-9d97-00a0c90a43cb"},
        {"CLSID_DirectPlayLobby", "2fe8f810-b2a5-11d0-a787-0000f803abfc"},
        {"IID_IDirectPlay2", "2b74f7c0-9154-11cf-a9cd-00aa006886e3"},
        {"IID_IDirectPlay2A", "9d460580-a822-11cf-960c-0080c7534e82"},
        {"IID_IDirectPlay3", "133efe40-32dc-11d0-9cfb-00a0c90a43cb"},
        {"IID_IDirectPlay3A", "133efe41-32dc-11d0-9cfb-00a0c90a43cb"},
        {"IID_IDirectPlayLobby", "af465c71-9588-11cf-a020-00aa006157ac"},
        {"IID_IDirectPlayLobbyA", "26c66a70-b367-11cf-a024-00aa006157ac"},
        {"IID_IDirectPlayLobby2", "0194c220-a303-11d0-9c4f-00a0c905425e"},
        {"IID_IDirectPlayLobby2A", "1bb4af80-a303-11d0-9c4f-00a0c905425e"},
        {"DPSPGUID_IPX", "685bc400-9d2c-11cf-a9cd-00aa006886e3"},
        {"DPSPGUID_TCPIP", "36e95ee0-8577-11cf-960c-0080c7534e82"},
        {"DPSPGUID_SERIAL", "0f1d6860-88d9-11cf-9c4e-00a0c905425e"},
        {"DPSPGUID_MODEM", "44eaa760-cb68-11cf-9c4e-00a0c905425e"},
        {"APP_GUID_Cossacks", "5bfdb060-06a4-11d0-9c4f-00a0c705475f"},
        {"DPAID_ServiceProvider", "07d916c0-e0af-11cf-9c4e-00a0c905425e"},
        {"DPAID_INet", "c4a54da0-e0af-11cf-9c4e-00a0c905425e"},
    };

    static byte[] guidBytes(String s) {
        String h = s.replace("-", "");
        byte[] b = new byte[16];
        for (int i = 0; i < 16; i++) b[i] = (byte) Integer.parseInt(h.substring(i * 2, i * 2 + 2), 16);
        // Data1 (4), Data2 (2), Data3 (2) are little-endian in memory
        byte[] r = b.clone();
        r[0] = b[3]; r[1] = b[2]; r[2] = b[1]; r[3] = b[0];
        r[4] = b[5]; r[5] = b[4];
        r[6] = b[7]; r[7] = b[6];
        return r;
    }

    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "dplay_users.txt";
        Map<Function, Set<String>> users = new LinkedHashMap<>();

        for (String[] g : GUIDS) {
            Address a = currentProgram.getMemory().findBytes(
                currentProgram.getMinAddress(), guidBytes(g[1]), null, true, monitor);
            if (a == null) { println(g[0] + ": not found"); continue; }
            createLabel(a, g[0], true);
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) users.computeIfAbsent(f, k -> new TreeSet<>()).add(g[0] + "@" + r.getFromAddress());
            }
        }
        for (Symbol s : currentProgram.getSymbolTable().getExternalSymbols()) {
            String n = s.getName();
            boolean dplay = s.getParentNamespace().getName().equalsIgnoreCase("DPLAYX.DLL");
            if (!dplay && !n.equals("CoCreateInstance") && !n.equals("CoInitialize")) continue;
            for (Reference r : s.getReferences()) {
                // import thunk pointer: follow references to the IAT slot too
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) users.computeIfAbsent(f, k -> new TreeSet<>()).add(n + "@" + r.getFromAddress());
                for (Reference r2 : getReferencesTo(r.getFromAddress())) {
                    Function f2 = getFunctionContaining(r2.getFromAddress());
                    if (f2 != null) users.computeIfAbsent(f2, k -> new TreeSet<>()).add(n + "@" + r2.getFromAddress());
                }
            }
        }

        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(out)) {
            for (Map.Entry<Function, Set<String>> e : users.entrySet()) {
                Function f = e.getKey();
                w.println("=================================================================");
                w.println(f.getName() + " @ " + f.getEntryPoint() + "  uses: " + e.getValue());
                w.println("callers: " + f.getCallingFunctions(monitor));
                DecompileResults res = d.decompileFunction(f, 60, monitor);
                w.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "<decompile failed>");
            }
        }
        println("functions: " + users.size() + " -> " + out);
    }
}
