#include "globals.h"
#include "visitor.h"

using namespace clang;

// ------------------------------
// Precompiler options
// ------------------------------


llvm::cl::OptionCategory SkePUCategory("SkePU precompiler options");

llvm::cl::opt<std::string> ResultDir("dir", llvm::cl::desc("Directory of output files"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<std::string> ResultName("name", llvm::cl::desc("File name of main output file (without extension, e.g., .cpp or .cu)"), llvm::cl::cat(SkePUCategory));

llvm::cl::opt<bool> GenCUDA("cuda",  llvm::cl::desc("Generate CUDA backend"),   llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> GenOMP("openmp", llvm::cl::desc("Generate OpenMP backend"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> GenCL("opencl",  llvm::cl::desc("Generate OpenCL backend"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> GenStarPUMPI("starpu-mpi",  llvm::cl::desc("Generate StarPU-MPI backend"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> GenMPI("mpi",  llvm::cl::desc("Generate MPI backend"), llvm::cl::cat(SkePUCategory));

llvm::cl::opt<bool> Verbose("verbose",  llvm::cl::desc("Verbose logging printout"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> Silent("silent",  llvm::cl::desc("Disable normal printouts"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> NoAddExtension("override-extension",  llvm::cl::desc("Do not automatically add file extension to output file (good for headers)"), llvm::cl::cat(SkePUCategory));

llvm::cl::opt<bool> DoNotGenLineDirectives("no-preserve-lines", llvm::cl::desc("Do not try to preserve line numbers from source file"),   llvm::cl::cat(SkePUCategory));

llvm::cl::opt<std::string> AllowedFuncNames("fnames", llvm::cl::desc("Function names which are allowed to be called from user functions (separated by space, e.g. -fnames \"conj csqrt\")"), llvm::cl::cat(SkePUCategory));

// Derived
static std::string mainFileName;
std::string inputFileName;


// ------------------------------
// Globals
// ------------------------------

// User functions, name maps to AST entry and indexed indicator
std::unordered_map<const FunctionDecl*, UserFunction*> UserFunctions;

// User functions, name maps to AST entry and indexed indicator
std::unordered_map<const TypeDecl*, UserType*> UserTypes;

// User functions, name maps to AST entry and indexed indicator
std::unordered_map<const VarDecl*, UserConstant*> UserConstants;

// Explicitly allowed functions to call from user functions
std::unordered_set<std::string> AllowedFunctionNamesCalledInUFs
{
	"exp", "exp2", "exp2f",
	"sqrt",
	"abs", "fabs",
	"max", "fmax",
	"pow",
	"log", "log2", "log10",
	"sin", "sinh", "asin", "asinh",
	"cos", "cosh", "acos", "acosh",
	"tan", "tanh", "atan", "atanh",
	"round", "ceil", "floor",
	"erf",
	"printf",
};

// Skeleton types lookup from internal SkePU class template name
const std::unordered_map<std::string, Skeleton> Skeletons =
{
	{"MapImpl",              {"Map",                Skeleton::Type::Map,                1, 1}},
	{"Reduce1D",             {"Reduce1D",           Skeleton::Type::Reduce1D,           1, 1}},
	{"Reduce2D",             {"Reduce2D",           Skeleton::Type::Reduce2D,           2, 2}},
	{"MapReduceImpl",        {"MapReduce",          Skeleton::Type::MapReduce,          2, 2}},
	{"ScanImpl",             {"Scan",               Skeleton::Type::Scan,               1, 3}},
	{"MapOverlap1D",         {"MapOverlap1D",       Skeleton::Type::MapOverlap1D,       1, 4}},
	{"MapOverlap2D",         {"MapOverlap2D",       Skeleton::Type::MapOverlap2D,       1, 1}},
	{"MapOverlap3D",         {"MapOverlap3D",       Skeleton::Type::MapOverlap3D,       1, 1}},
	{"MapOverlap4D",         {"MapOverlap4D",       Skeleton::Type::MapOverlap4D,       1, 1}},
	{"MapPairsImpl",         {"MapPairs",           Skeleton::Type::MapPairs,           1, 1}},
	{"MapPairsReduceImpl",   {"MapPairsReduce",     Skeleton::Type::MapPairsReduce,     2, 1}},
	{"CallImpl",             {"Call",               Skeleton::Type::Call,               1, 1}},
};

Rewriter GlobalRewriter;
size_t GlobalSkeletonIndex = 0;

// Library markers
bool didFindBlas = false;
clang::SourceLocation blasBegin, blasEnd;



llvm::raw_ostream& SkePULog()
{
	if (Verbose)
		return llvm::outs();
	else
		return llvm::nulls();
}


// For each source file provided to the tool, a new FrontendAction is created.
class SkePUFrontendAction : public ASTFrontendAction
{
public:

	bool BeginSourceFileAction(CompilerInstance &CI) override
	{
		inputFileName = this->getCurrentFile().str();
		if (Verbose) SkePULog() << "** BeginSourceFileAction for: " << inputFileName << "\n";
		return true;
	}
	
	void EndSourceFileAction() override
	{
		SourceManager &SM = GlobalRewriter.getSourceMgr();
		SourceLocation SLStart = SM.getLocForStartOfFile(SM.getMainFileID());

		GlobalRewriter.InsertText(SLStart, "#define SKEPU_PRECOMPILED 1\n");
		if (GenOMP)  GlobalRewriter.InsertText(SLStart, "#define SKEPU_OPENMP 1\n");
		if (GenCL)   GlobalRewriter.InsertText(SLStart, "#define SKEPU_OPENCL 1\n");
		if (GenCUDA) GlobalRewriter.InsertText(SLStart, "#define SKEPU_CUDA 1\n");
		if (GenStarPUMPI) GlobalRewriter.InsertText(SLStart, "#define SKEPU_STARPU_MPI 1\n");
		if (GenMPI) GlobalRewriter.InsertText(SLStart, "#define SKEPU_MPI 1\n");
//		if (!DoNotGenLineDirectives)
//			GlobalRewriter.InsertText(SLStart, "#line 1 \"" + inputFileName + "\"\n");
		
		for (VarDecl *d : this->SkeletonInstances)
			HandleSkeletonInstance(d);
		
		if (didFindBlas)
		{
			std::string blasTransformedCode = GlobalRewriter.getRewrittenText(SourceRange(blasBegin, blasEnd));
			blasTransformedCode = "\n/* BEGIN BLAS.HPP INJECTION */\n//" + blasTransformedCode + ";\n/* END BLAS.HPP INJECTION*/\n";
			
			
			FileID blasFile = GlobalRewriter.getSourceMgr().getFileID(blasBegin);
			SourceLocation blasIncludeLoc = GlobalRewriter.getSourceMgr().getIncludeLoc(blasFile);
			unsigned blasIncludeLine = GlobalRewriter.getSourceMgr().getSpellingLineNumber(blasIncludeLoc);
			blasIncludeLoc = GlobalRewriter.getSourceMgr().translateLineCol(SM.getMainFileID(), blasIncludeLine, 0);
			
			GlobalRewriter.InsertText(blasIncludeLoc, blasTransformedCode);
		}

		if (Verbose) SkePULog() << "** EndSourceFileAction for: " << inputFileName << "\n";

		// Now emit the rewritten buffer.
		std::error_code EC;
		llvm::raw_fd_ostream OutFile(
			mainFileName, EC, llvm::sys::fs::FileAccess::FA_Write);
		GlobalRewriter.getEditBuffer(SM.getMainFileID()).write(OutFile);
		OutFile.close();
	}

	std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override
	{
		if (Verbose) SkePULog() << "** Creating AST consumer for: " << file << "\n";
		GlobalRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
		return llvm::make_unique<SkePUASTConsumer>(&CI.getASTContext(), this->SkeletonInstances);
	}

private:
	std::unordered_set<clang::VarDecl *> SkeletonInstances;
};


int main(int argc, const char **argv)
{
	tooling::CommonOptionsParser op(argc, argv, SkePUCategory);
	tooling::ClangTool Tool(op.getCompilations(), op.getSourcePathList());

	if (ResultName == "")
		ResultName = op.getSourcePathList()[0];
	mainFileName = ResultDir + "/" + ResultName + (NoAddExtension ? "" : (GenCUDA ? ".cu" : ".cpp"));

	if (!Silent)
	{
		SkePULog() << "# ======================================= #\n";
		SkePULog() << "~   SkePU source-to-source compiler v3    ~\n";
		SkePULog() << "# --------------------------------------- #\n";
		SkePULog() << "   OpenMP gen:       " << (GenOMP ? "ON" : "OFF") << "\n";
		SkePULog() << "   CUDA gen:         " << (GenCUDA ? "ON" : "OFF") << "\n";
		SkePULog() << "   OpenCL gen:       " << (GenCL ? "ON" : "OFF") << "\n";
		SkePULog() << "   StarPU-MPI gen:   " << (GenStarPUMPI ? "ON" : "OFF") << "\n";
		SkePULog() << "   Main output file: " << mainFileName << "\n";
		SkePULog() << "# ======================================= #\n";
	}

	std::istringstream SSNames(AllowedFuncNames);
	std::vector<std::string> Names{std::istream_iterator<std::string>{SSNames}, std::istream_iterator<std::string>{}};
	for (std::string &name : Names)
		AllowedFunctionNamesCalledInUFs.insert(name);

	return Tool.run(tooling::newFrontendActionFactory<SkePUFrontendAction>().get());
}
