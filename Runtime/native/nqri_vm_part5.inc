namespace noqeri::nqri {
static int diffNqriFiles(const std::string& firstPath, const std::string& secondPath) {
    auto first = loadNqri2(readFile(firstPath));
    auto second = loadNqri2(readFile(secondPath));
    if (first.bytes == second.bytes) {
        std::cout << "NQRI modules are byte-identical\n";
        return 0;
    }
    std::size_t index = 0;
    while (index < first.bytes.size() && index < second.bytes.size() &&
           first.bytes[index] == second.bytes[index]) ++index;
    std::cerr << "NQRI mismatch: first differing 0-based offset=" << index
              << " bytes=" << first.bytes.size() << "/" << second.bytes.size() << "\n";
    if (index >= first.bytes.size() || index >= second.bytes.size()) return 1;
    std::cerr << "  actual bytes: " << static_cast<unsigned>(first.bytes[index])
              << " / " << static_cast<unsigned>(second.bytes[index]) << "\n";
    const std::size_t functionStart = 32 + first.source.size();
    const std::size_t instructionStart = functionStart + first.functions.size() * 40;
    if (index < 32) { std::cerr << "  section=header\n"; return 1; }
    if (index < functionStart) { std::cerr << "  section=source; offset=" << index - 32 << "\n"; return 1; }
    if (index < instructionStart) {
        std::cerr << "  section=function table; record=" << (index - functionStart) / 40
                  << " recordByte=" << (index - functionStart) % 40 << "\n";
        return 1;
    }
    const std::size_t pc = (index - instructionStart) / 96;
    if (pc >= first.instructions.size() || pc >= second.instructions.size()) return 1;
    const auto& a = first.instructions[pc], &b = second.instructions[pc];
    const std::size_t fieldOffset = (index - instructionStart) % 96;
    std::cerr << "  section=instruction; pc=" << pc << " recordByte=" << fieldOffset << "\n";
    for (const auto& fn : first.functions) {
        if (pc >= fn.firstInstruction && pc - fn.firstInstruction < fn.instructionCount) {
            std::cerr << "  function=" << fn.name << " localPC=" << pc - fn.firstInstruction << "\n";
            break;
        }
    }
    auto describe = [](const Instruction& ins) {
        std::cerr << "  opcode=" << ins.opcode << " type=" << ins.typeCode
                  << " flags=" << ins.flags << " dest=" << ins.dest
                  << " left=" << ins.left << " right=" << ins.right
                  << " immediate=" << ins.immediate << " symbol=" << ins.symbol
                  << " width=" << ins.width << " source=" << ins.sourceStart
                  << ":" << ins.sourceEnd << "\n";
    };
    std::cerr << "  Stage-2:"; describe(a);
    std::cerr << "  Stage-3:"; describe(b);
    const auto text = spanText(first, a);
    if (!text.empty()) std::cerr << "  source span=" << text.substr(0, 200) << "\n";
    return 1;
}
} // namespace noqeri::nqri

int main(int argc,char**argv){using namespace noqeri::nqri;try{if(argc==2&&std::string(argv[1])=="--selftest")return selftest();if(argc<2){std::cerr<<"usage:\n  noqeri-nqri --selftest\n  noqeri-nqri <module.nqri> [function] [integer-args...]\n  noqeri-nqri has-export <module.nqri> <name>\n  noqeri-nqri diff <stage2.nqri> <stage3.nqri>\n  noqeri-nqri syntax <compiler.nqri> <source.noqeri>\n  noqeri-nqri check <compiler.nqri> <source.noqeri> [target]\n  noqeri-nqri compile <compiler.nqri> <merged-source.noqeri> <output.nqri> [target]\n  noqeri-nqri compile-manifest <compiler.nqri> <repo-root> <manifest> <output.nqri> [target]\n  noqeri-nqri compile-native <compiler.nqri> <source.noqeri> <output.s> [target]\n  noqeri-nqri compile-native-manifest <compiler.nqri> <repo-root> <manifest> <output.s> [target]\n";return 2;}const std::string command=argv[1];if(command=="diff"){if(argc!=4)throw RuntimeError("diff requires two NQRI modules");return diffNqriFiles(argv[2],argv[3]);}if(command=="has-export"){if(argc!=4)throw RuntimeError("has-export requires <module.nqri> <name>");auto module=loadNqri2(readFile(argv[2]));auto found=module.byName.find(argv[3]);if(found!=module.byName.end() && (module.functions[found->second].flags & 1u)!=0u){std::cout<<argv[3]<<"\n";return 0;}std::cerr<<"missing exported NQRI symbol: "<<argv[3]<<"\n";return 1;}if(command=="compile-native"||command=="compile-native-manifest"){const bool fromManifest=command=="compile-native-manifest";if((!fromManifest&&argc<5)||(fromManifest&&argc<6))throw RuntimeError("insufficient native compile arguments");const std::string modulePath=argv[2];auto module=loadNqri2(readFile(modulePath));Vm vm(std::move(module),fromManifest?6000000000ull:250000000ull);const std::string sourcePath=fromManifest?argv[4]:argv[3];auto source=fromManifest?bundleManifest(argv[3],argv[4]):readFile(sourcePath);auto sourceSlice=vm.bytesSlice(source);const std::int64_t target=fromManifest?(argc>=7?std::stoll(argv[6]):3):(argc>=6?std::stoll(argv[5]):3);const std::size_t outputCapacity=128ull*1024ull*1024ull;auto output=vm.writableBytes(outputCapacity);auto result=vm.invoke("stage2CompileToNativeAssembly",{sourceSlice,target,std::int64_t{0},std::int64_t{1},output});const auto written=asInt(result);if(written<0){std::size_t offset=0;try{offset=checkedSize(asInt(vm.invoke("stage2SemanticErrorOffset",{sourceSlice,std::int64_t{1}})),"native semantic error offset");}catch(const std::exception&){offset=0;}std::size_t line=1,column=1;for(std::size_t i=0;i<offset&&i<source.size();++i){if(source[i]==10){++line;column=1;}else{++column;}}std::cerr<<sourcePath<<":"<<line<<":"<<column<<": native Stage-2 compile failed ("<<written<<", byte "<<offset<<")\n";return 1;}auto bytes=vm.copyBytes(output,checkedSize(written,"compiled native assembly size"));const std::string outputPath=fromManifest?argv[5]:argv[4];writeFile(outputPath,bytes,bytes.size());std::cout<<"Wrote Noqeri-native x86-64 assembly: "<<outputPath<<" ("<<bytes.size()<<" bytes)\n";return 0;}if(command=="syntax"||command=="check"||command=="compile"||command=="compile-manifest"){if((command=="syntax"&&argc<4)||(command=="check"&&argc<4)||(command=="compile"&&argc<5)||(command=="compile-manifest"&&argc<6))throw RuntimeError("insufficient arguments");const std::string modulePath=argv[2];auto module=loadNqri2(readFile(modulePath));const bool fromManifest=command=="compile-manifest";const std::uint64_t stepBudget=fromManifest?4000000000ull:50000000ull;Vm vm(std::move(module),stepBudget);const std::string sourcePath=fromManifest?argv[4]:argv[3];auto source=fromManifest?bundleManifest(argv[3],argv[4]):readFile(sourcePath);auto sourceSlice=vm.bytesSlice(source);if(command=="syntax"){auto result=vm.invoke("stage2SyntaxCheck",{sourceSlice});auto status=asInt(result);if(status!=0){std::size_t offset=0;try{offset=checkedSize(asInt(vm.invoke("stage2SyntaxErrorOffset",{sourceSlice})),"syntax error offset");}catch(const std::exception&){offset=0;}std::size_t line=1,column=1;for(std::size_t i=0;i<offset&&i<source.size();++i){if(source[i]==10){++line;column=1;}else{++column;}}std::cerr<<sourcePath<<":"<<line<<":"<<column<<": Stage-2 syntax failed ("<<status<<", byte "<<offset<<")\n";return 1;}std::cout<<sourcePath<<": Stage-2 syntax OK\n";return 0;}const std::int64_t target=(command=="check"?(argc>=5?std::stoll(argv[4]):3):(fromManifest?(argc>=7?std::stoll(argv[6]):3):(argc>=6?std::stoll(argv[5]):3)));if(command=="check"){auto result=vm.invoke("stage2CompileCheck",{sourceSlice,target,std::int64_t{0}});auto status=asInt(result);if(status!=0){std::cerr<<sourcePath<<": Stage-2 check failed ("<<status<<")";if(status==-704){try{const auto packed=asInt(vm.invoke("stage2InvalidNirDiagnostic",{sourceSlice,target,std::int64_t{0}}));if(packed>0){const auto reason=packed%64;const auto rest=packed/64;const auto opcode=rest%64;const auto index=rest/64-1;std::cerr<<" invalid-nir-index="<<index<<" opcode="<<opcode<<" reason="<<reason;}}catch(const std::exception&){} }std::cerr<<"\n";return 1;}std::cout<<sourcePath<<": Stage-2 check OK\n";return 0;}const std::size_t maxRecords=32u+source.size()+8192u*40u+262144u*96u+262144u;auto output=vm.writableBytes(maxRecords);auto result=vm.invoke("stage2CompileToNqri",{sourceSlice,target,fromManifest?std::int64_t{1}:std::int64_t{0},fromManifest?std::int64_t{1}:std::int64_t{0},output});const auto written=asInt(result);if(written<0){std::size_t offset=0;try{offset=checkedSize(asInt(vm.invoke("stage2SemanticErrorOffset",{sourceSlice,fromManifest?std::int64_t{1}:std::int64_t{0}})),"semantic error offset");}catch(const std::exception&){offset=0;}std::size_t line=1,column=1;for(std::size_t i=0;i<offset&&i<source.size();++i){if(source[i]==10){++line;column=1;}else{++column;}}std::cerr<<sourcePath<<":"<<line<<":"<<column<<": Stage-2 NQRI compile failed ("<<written<<", byte "<<offset<<")";if(written==-704){try{const auto packed=asInt(vm.invoke("stage2InvalidNirDiagnostic",{sourceSlice,target,fromManifest?std::int64_t{1}:std::int64_t{0}}));if(packed>0)std::cerr<<" invalid-nir-index="<<(packed/64-1)<<" opcode="<<(packed%64);}catch(const std::exception&){} }std::cerr<<"\n";return 1;}auto bytes=vm.copyBytes(output,checkedSize(written,"compiled NQRI size"));const std::string outputPath=fromManifest?argv[5]:argv[4];writeFile(outputPath,bytes,bytes.size());std::cout<<"Wrote native NQRI compiler output: "<<outputPath<<" ("<<bytes.size()<<" bytes)\n";return 0;}auto module=loadNqri2(readFile(argv[1]));Vm vm(std::move(module));const std::string functionName=argc>=3?argv[2]:"main";std::vector<Value>args;for(int i=3;i<argc;++i)args.emplace_back(static_cast<std::int64_t>(std::stoll(argv[i])));auto result=vm.invoke(functionName,args);std::cout<<valueText(result)<<'\n';return 0;}catch(const ThrowSignal&t){std::cerr<<"Noqeri throw: "<<t.value<<'\n';return 1;}catch(const std::exception&e){std::cerr<<"noqeri-nqri: "<<e.what()<<'\n';return 1;}}
