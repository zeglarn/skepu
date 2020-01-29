#include "code_gen.h"

using namespace clang;

// ------------------------------
// Kernel templates
// ------------------------------

const char *MapPairsKernelTemplate_CL = R"~~~(
__kernel void SKEPU_KERNEL_NAME(SKEPU_KERNEL_PARAMS __global SKEPU_MAP_RESULT_TYPE* output, size_t w, size_t n, size_t Vsize, size_t Hsize, size_t base)
{
	size_t i = get_global_id(0);
	size_t gridSize = get_local_size(0) * get_num_groups(0);
	SKEPU_CONTAINER_PROXIES
	
	while (i < n)
	{
		SKEPU_INDEX_INITIALIZER
		SKEPU_CONTAINER_PROXIE_INNER
		output[i] = SKEPU_FUNCTION_NAME_MAP(SKEPU_MAP_PARAMS);
		i += gridSize;
	}
}
)~~~";


const std::string Constructor = R"~~~(
class SKEPU_KERNEL_CLASS
{
public:
	
	static cl_kernel kernels(size_t deviceID, cl_kernel *newkernel = nullptr)
	{
		static cl_kernel arr[8]; // Hard-coded maximum
		if (newkernel)
		{
			arr[deviceID] = *newkernel;
			return nullptr;
		}
		else return arr[deviceID];
	}
	
	static void initialize()
	{
		static bool initialized = false;
		if (initialized)
			return;
		
		std::string source = skepu::backend::cl_helpers::replaceSizeT(R"###(SKEPU_OPENCL_KERNEL)###");
		
		// Builds the code and creates kernel for all devices
		size_t counter = 0;
		for (skepu::backend::Device_CL *device : skepu::backend::Environment<int>::getInstance()->m_devices_CL)
		{
			cl_int err;
			cl_program program = skepu::backend::cl_helpers::buildProgram(device, source);
			cl_kernel kernel = clCreateKernel(program, "SKEPU_KERNEL_NAME", &err);
			CL_CHECK_ERROR(err, "Error creating mappairs kernel 'SKEPU_KERNEL_NAME'");
			
			kernels(counter++, &kernel);
		}
		
		initialized = true;
	}
	
	static void map
	(
		size_t deviceID, size_t localSize, size_t globalSize,
		SKEPU_HOST_KERNEL_PARAMS skepu::backend::DeviceMemPointer_CL<SKEPU_MAP_RESULT_TYPE> *output,
		size_t w, size_t n, size_t Vsize, size_t Hsize, size_t base
	)
	{
		skepu::backend::cl_helpers::setKernelArgs(kernels(deviceID), SKEPU_KERNEL_ARGS output->getDeviceDataPointer(), w, n, Vsize, Hsize, base);
		cl_int err = clEnqueueNDRangeKernel(skepu::backend::Environment<int>::getInstance()->m_devices_CL.at(deviceID)->getQueue(), kernels(deviceID), 1, NULL, &globalSize, &localSize, 0, NULL, NULL);
		CL_CHECK_ERROR(err, "Error launching Map kernel");
	}
};
)~~~";


std::string createMapPairsKernelProgram_CL(UserFunction &mapFunc, size_t Varity, size_t Harity, std::string dir)
{
	std::stringstream sourceStream, SSMapFuncParams, SSKernelParamList, SSHostKernelParamList, SSKernelArgs, SSProxyInitializer, SSProxyInitializerInner;
	std::string indexInitializer;
	std::map<ContainerType, std::set<std::string>> containerProxyTypes;
	bool first = true;
	
	if (mapFunc.indexed1D)
	{
		SSMapFuncParams << "index";
		indexInitializer = "index1_t index = { .i = base + i };";
		first = false;
	}
	else if (mapFunc.indexed2D)
	{
		SSMapFuncParams << "index";
		indexInitializer = "index2_t index = { .row = (base + i) / w, .col = (base + i) % w };";
		first = false;
	}
	
	size_t ctr = 0;
	for (UserFunction::Param& param : mapFunc.elwiseParams)
	{
		if (!first) { SSMapFuncParams << ", "; }
		SSKernelParamList << "__global " << param.resolvedTypeName << " *" << param.name << ", ";
		SSHostKernelParamList << "skepu::backend::DeviceMemPointer_CL<" << param.resolvedTypeName << "> *" << param.name << ", ";
		SSKernelArgs << param.name << "->getDeviceDataPointer(), ";
		if (ctr++ < mapFunc.Varity) // vertical containers
			SSMapFuncParams << param.name << "[i / Hsize]";
		else // horizontal containers
			SSMapFuncParams << param.name << "[i % Hsize]";
		first = false;
	}
	
	for (UserFunction::RandomAccessParam& param : mapFunc.anyContainerParams)
	{
		std::string name = "skepu_container_" + param.name;
		if (!first) { SSMapFuncParams << ", "; }
		SSHostKernelParamList << param.TypeNameHost() << " skepu_container_" << param.name << ", ";
		containerProxyTypes[param.containerType].insert(param.resolvedTypeName);
		switch (param.containerType)
		{
			case ContainerType::Vector:
				SSKernelParamList << "__global " << param.resolvedTypeName << " *" << name << ", size_t skepu_size_" << param.name << ", ";
				SSKernelArgs << "std::get<1>(" << name << ")->getDeviceDataPointer(), std::get<0>(" << name << ")->size(), ";
				SSProxyInitializer << param.TypeNameOpenCL() << " " << param.name << " = { .data = " << name << ", .size = skepu_size_" << param.name << " };\n";
				break;
			
			case ContainerType::Matrix:
				SSKernelParamList << "__global " << param.resolvedTypeName << " *" << name << ", size_t skepu_rows_" << param.name << ", size_t skepu_cols_" << param.name << ", ";
				SSKernelArgs << "std::get<1>(" << name << ")->getDeviceDataPointer(), std::get<0>(" << name << ")->total_rows(), std::get<0>(" << name << ")->total_cols(), ";
				SSProxyInitializer << param.TypeNameOpenCL() << " " << param.name << " = { .data = " << name
					<< ", .rows = skepu_rows_" << param.name << ", .cols = skepu_cols_" << param.name << " };\n";
				break;
			
			case ContainerType::MatRow:
				SSKernelParamList << "__global " << param.resolvedTypeName << " *" << name << ", size_t skepu_cols_" << param.name << ", ";
				SSKernelArgs << "std::get<1>(" << name << ")->getDeviceDataPointer(), std::get<0>(" << name << ")->total_cols(), ";
				SSProxyInitializerInner << param.TypeNameOpenCL() << " " << param.name << " = { .data = (" << name << " + i * skepu_cols_" << param.name << "), .cols = skepu_cols_" << param.name << " };\n";
				break;
			
			case ContainerType::Tensor3:
				SSKernelParamList << "__global " << param.resolvedTypeName << " *" << name << ", "
					<< "size_t skepu_size_i_" << param.name << ", size_t skepu_size_j_" << param.name << ", size_t skepu_size_k_" << param.name << ", ";
				SSKernelArgs << "std::get<1>(" << name << ")->getDeviceDataPointer(), "
					<< "std::get<0>(" << name << ")->size_i(), std::get<0>(" << name << ")->size_j(), std::get<0>(" << name << ")->size_k(), ";
				SSProxyInitializer << param.TypeNameOpenCL() << " " << param.name << " = { .data = " << name
					<< ", .size_i = skepu_size_i_" << param.name << ", .size_j = skepu_size_j_" << param.name << ", .size_k = skepu_size_k_" << param.name << " };\n";
				break;
			
			case ContainerType::Tensor4:
				SSKernelParamList << "__global " << param.resolvedTypeName << " *" << name << ", "
					<< "size_t skepu_size_i_" << param.name << ", size_t skepu_size_j_" << param.name << ", size_t skepu_size_k_" << param.name << ", size_t skepu_size_l_" << param.name << ", ";
				SSKernelArgs << "std::get<1>(" << name << ")->getDeviceDataPointer(), "
					<< "std::get<0>(" << name << ")->size_i(), std::get<0>(" << name << ")->size_j(), std::get<0>(" << name << ")->size_k(), std::get<0>(" << name << ")->size_l(), ";
				SSProxyInitializer << param.TypeNameOpenCL() << " " << param.name << " = { .data = " << name
					<< ", .size_i = skepu_size_i_" << param.name << ", .size_j = skepu_size_j_" << param.name << ", .size_k = skepu_size_k_" << param.name << ", .size_l = skepu_size_l_" << param.name << " };\n";
				break;
			
			case ContainerType::SparseMatrix:
				SSKernelParamList
					<< "__global " << param.resolvedTypeName << " *" << name << ", "
					<< "__global size_t *" << param.name << "_row_pointers, "
					<< "__global size_t *" << param.name << "_col_indices, "
					<< "size_t skepu_size_" << param.name << ", ";
					
				SSKernelArgs
					<< "std::get<1>(" << name << ")->getDeviceDataPointer(), "
					<< "std::get<2>(" << name << ")->getDeviceDataPointer(), "
					<< "std::get<3>(" << name << ")->getDeviceDataPointer(), "
					<< "std::get<0>(" << name << ")->total_nnz(), ";
				
				SSProxyInitializer << param.TypeNameOpenCL() << " " << param.name << " = { "
					<< ".data = " << name << ", "
					<< ".row_offsets = " << param.name << "_row_pointers, "
					<< ".col_indices = " << param.name << "_col_indices, "
					<< ".count = skepu_size_" << param.name << " };\n";
				break;
		}
		SSMapFuncParams << param.name;
		first = false;
	}
	
	for (UserFunction::Param& param : mapFunc.anyScalarParams)
	{
		if (!first) { SSMapFuncParams << ", "; }
		SSKernelParamList << param.resolvedTypeName << " " << param.name << ", ";
		SSHostKernelParamList << param.resolvedTypeName << " " << param.name << ", ";
		SSKernelArgs << param.name << ", ";
		SSMapFuncParams << param.name;
		first = false;
	}
	
	if (mapFunc.requiresDoublePrecision)
		sourceStream << "#pragma OPENCL EXTENSION cl_khr_fp64: enable\n";
	
	for (const std::string &type : containerProxyTypes[ContainerType::Vector])
		sourceStream << generateOpenCLVectorProxy(type);

	for (const std::string &type : containerProxyTypes[ContainerType::Matrix])
		sourceStream << generateOpenCLMatrixProxy(type);

	for (const std::string &type : containerProxyTypes[ContainerType::SparseMatrix])
		sourceStream << generateOpenCLSparseMatrixProxy(type);
	
	for (const std::string &type : containerProxyTypes[ContainerType::MatRow])
		sourceStream << generateOpenCLMatrixRowProxy(type);
	
	for (const std::string &type : containerProxyTypes[ContainerType::Tensor3])
		sourceStream << generateOpenCLTensor3Proxy(type);
	
	for (const std::string &type : containerProxyTypes[ContainerType::Tensor4])
		sourceStream << generateOpenCLTensor4Proxy(type);
	
	// Include user constants as preprocessor macros
	for (auto pair : UserConstants)
		sourceStream << "#define " << pair.second->name << " (" << pair.second->definition << ") // " << pair.second->typeName << "\n";
	
	// check for extra user-supplied opencl code for custome datatype
	// TODO: Also check the referenced UFs for referenced UTs skepu::userstruct
	for (UserType *RefType : mapFunc.ReferencedUTs)
		sourceStream << generateUserTypeCode_CL(*RefType);
	
	std::stringstream SSKernelName;
	SSKernelName << transformToCXXIdentifier(ResultName) << "_MapPairsKernel_" << mapFunc.uniqueName << "_Varity_" << Varity << "_Harity_" << Harity;
	const std::string kernelName = SSKernelName.str();
	const std::string className = "CLWrapperClass_" + kernelName;
	
	sourceStream << KernelPredefinedTypes_CL << generateUserFunctionCode_CL(mapFunc) << MapPairsKernelTemplate_CL;
	
	std::string finalSource = Constructor;
	replaceTextInString(finalSource, "SKEPU_OPENCL_KERNEL", sourceStream.str());
	replaceTextInString(finalSource, PH_MapResultType, mapFunc.resolvedReturnTypeName);
	replaceTextInString(finalSource, PH_KernelName, kernelName);
	replaceTextInString(finalSource, PH_MapFuncName, mapFunc.uniqueName);
	replaceTextInString(finalSource, PH_KernelParams, SSKernelParamList.str());
	replaceTextInString(finalSource, "SKEPU_HOST_KERNEL_PARAMS", SSHostKernelParamList.str());
	replaceTextInString(finalSource, PH_MapParams, SSMapFuncParams.str());
	replaceTextInString(finalSource, PH_IndexInitializer, indexInitializer);
	replaceTextInString(finalSource, "SKEPU_KERNEL_CLASS", className);
	replaceTextInString(finalSource, "SKEPU_KERNEL_ARGS", SSKernelArgs.str());
	replaceTextInString(finalSource, "SKEPU_CONTAINER_PROXIES", SSProxyInitializer.str());
	replaceTextInString(finalSource, "SKEPU_CONTAINER_PROXIE_INNER", SSProxyInitializerInner.str());
	
	std::ofstream FSOutFile {dir + "/" + kernelName + "_cl_source.inl"};
	FSOutFile << finalSource;
	
	return kernelName;
}