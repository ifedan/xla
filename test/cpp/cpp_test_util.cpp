#include "cpp_test_util.h"

#include <iostream>
#include <string>

#include "tensorflow/compiler/xla/xla_client/debug_macros.h"
#include "torch_xla/csrc/aten_xla_bridge.h"
#include "torch_xla/csrc/ir_dump_util.h"
#include "torch_xla/csrc/lowering_context.h"
#include "torch_xla/csrc/ops/device_data.h"
#include "torch_xla/csrc/tensor_util.h"
#include "torch_xla/csrc/torch_util.h"

namespace torch_xla {
namespace cpp_test {

at::Tensor ToCpuTensor(const at::Tensor& t) {
  // t.to() implicitly triggers a sync if t.device=torch::kXLA.
  return t.to(torch::kCPU);
}

bool EqualValues(at::Tensor tensor1, at::Tensor tensor2) {
  if (tensor1.sizes() != tensor2.sizes() ||
      tensor1.dtype() != tensor2.dtype()) {
    std::cerr << "Different shape:\n"
              << tensor1.dtype() << " " << tensor1.sizes() << "\n-vs-\n"
              << tensor2.dtype() << " " << tensor2.sizes() << "\n";
    return false;
  }
  tensor1 = ToCpuTensor(tensor1);
  tensor2 = ToCpuTensor(tensor2);

  at::ScalarType type1 = tensor1.scalar_type();
  at::ScalarType type2 = tensor2.scalar_type();
  if (type1 != type2) {
    tensor1 = tensor1.toType(type2);
  }
  return tensor1.equal(tensor2);
}

bool EqualValuesNoElementTypeCheck(at::Tensor tensor1, at::Tensor tensor2) {
  if (tensor1.sizes() != tensor2.sizes()) {
    std::cerr << "Different shape:\n"
              << tensor1.dtype() << " " << tensor1.sizes() << "\n-vs-\n"
              << tensor2.dtype() << " " << tensor2.sizes() << "\n";
    return false;
  }
  tensor1 = ToCpuTensor(tensor1);
  tensor2 = ToCpuTensor(tensor2);

  at::ScalarType type1 = tensor1.scalar_type();
  at::ScalarType type2 = tensor2.scalar_type();
  if (type1 != type2) {
    tensor1 = tensor1.toType(type2);
  }
  return tensor1.equal(tensor2);
}

void ForEachDevice(const std::function<void(const Device&)>& devfn) {
  std::string default_device =
      xla::ComputationClient::Get()->GetDefaultDevice();
  devfn(Device(default_device));
}

void ForEachDevice(const std::function<void(const torch::Device&)>& devfn) {
  torch::Device torch_device = bridge::AtenDefaultDevice();
  devfn(torch_device);
}

bool CloseValues(at::Tensor tensor1, at::Tensor tensor2, double rtol,
                 double atol) {
  if (tensor1.sizes() != tensor2.sizes() ||
      tensor1.dtype() != tensor2.dtype()) {
    std::cerr << "Different shape:\n"
              << tensor1.dtype() << " " << tensor1.sizes() << "\n-vs-\n"
              << tensor2.dtype() << " " << tensor2.sizes() << "\n";
    return false;
  }
  tensor1 = ToCpuTensor(tensor1);
  tensor2 = ToCpuTensor(tensor2);
  bool equal = tensor1.allclose(tensor2, rtol, atol);
  if (!equal) {
    std::cerr << tensor1 << "\n-vs-\n" << tensor2 << "\n";
  }
  return equal;
}

void WithAllDevices(
    DeviceType device_type,
    const std::function<void(const std::vector<Device>&,
                             const std::vector<Device>&)>& devfn) {
  std::vector<Device> devices;
  std::vector<Device> all_devices;
  for (const auto& device_str :
       xla::ComputationClient::Get()->GetLocalDevices()) {
    Device device(device_str);
    if (device.hw_type == device_type) {
      devices.push_back(device);
    }
  }
  for (const auto& device_str :
       xla::ComputationClient::Get()->GetAllDevices()) {
    Device device(device_str);
    if (device.hw_type == device_type) {
      all_devices.push_back(device);
    }
  }
  if (!devices.empty()) {
    devfn(devices, all_devices);
  }
}

std::string GetTensorTextGraph(at::Tensor tensor) {
  XLATensor xtensor = bridge::GetXlaTensor(tensor);
  return ir::DumpUtil::ToText({xtensor.GetIrValue().node.get()});
}

std::string GetTensorDotGraph(at::Tensor tensor) {
  XLATensor xtensor = bridge::GetXlaTensor(tensor);
  return ir::DumpUtil::ToDot({xtensor.GetIrValue().node.get()});
}

ir::Value GetTensorIrValue(const at::Tensor& tensor, const Device& device) {
  xla::ComputationClient::DataPtr data = TensorToXlaData(tensor, device);
  return ir::MakeNode<ir::ops::DeviceData>(std::move(data));
}

std::vector<xla::ComputationClient::DataPtr> Execute(
    tensorflow::gtl::ArraySlice<const ir::Value> roots, const Device& device) {
  ir::LoweringContext lowering_ctx("Execute");
  for (auto node : roots) {
    xla::XlaOp root = lowering_ctx.GetOutputOp(node);
    lowering_ctx.AddResult(root);
  }

  xla::XlaComputation computation = ConsumeValue(lowering_ctx.Build());
  xla::ProgramShape program_shape = ConsumeValue(computation.GetProgramShape());
  xla::Shape shape =
      MakeShapeWithDeviceLayout(program_shape.result(), device.hw_type);

  std::vector<xla::ComputationClient::CompileInstance> instances;
  instances.push_back({std::move(computation), device.ToString(),
                       xla::ComputationClient::Get()->GetCompilationDevices(
                           device.ToString(), {}),
                       &shape});

  std::vector<std::shared_ptr<xla::ComputationClient::Computation>>
      computations =
          xla::ComputationClient::Get()->Compile(std::move(instances));

  xla::ComputationClient::ExecuteComputationOptions options;
  return xla::ComputationClient::Get()->ExecuteComputation(
      *computations.front(), lowering_ctx.GetParametersData(),
      device.ToString(), options);
}

std::vector<at::Tensor> Fetch(
    tensorflow::gtl::ArraySlice<const xla::ComputationClient::DataPtr>
        device_data) {
  std::vector<xla::Literal> literals =
      xla::ComputationClient::Get()->TransferFromServer(device_data);
  std::vector<at::Tensor> tensors;
  for (auto& literal : literals) {
    tensors.push_back(MakeTensorFromXlaLiteral(
        literal, TensorTypeFromXlaType(literal.shape().element_type())));
  }
  return tensors;
}

std::vector<at::Tensor> ExecuteAndFetch(
    tensorflow::gtl::ArraySlice<const ir::Value> roots, const Device& device) {
  auto results = Execute(roots, device);
  return Fetch(results);
}

}  // namespace cpp_test
}  // namespace torch_xla
