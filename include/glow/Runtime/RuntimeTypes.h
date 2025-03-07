/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef GLOW_RUNTIME_RUNTIMETYPES_H
#define GLOW_RUNTIME_RUNTIMETYPES_H

#include "glow/Backend/Backend.h"
#include "glow/Backend/BackendUtils.h"
#include "glow/Backends/BackendOptions.h"
#include "glow/Graph/Graph.h"
#include "glow/Support/Error.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace glow {

class ExecutionContext;

namespace runtime {

class DeviceManager;
using DeviceIDTy = size_t;
using RunIdentifierTy = size_t;

/// Map of DeviceIDTy -> DeviceManager.
using DeviceManagerMapTy = std::map<DeviceIDTy, std::unique_ptr<DeviceManager>>;

/// Callback type used by HostManager and DeviceManager, used to pass results of
/// an inference request back to the caller.
using ResultCBTy = std::function<void(runtime::RunIdentifierTy, llvm::Error,
                                      std::unique_ptr<ExecutionContext>)>;

/// Data structure that contains device constraint information for each device.
/// Used to communicate memory constraints and later costs to the Partitioner.
struct DeviceInfo {
  /// Available memory on device in bytes.
  uint64_t availableMemory;
  /// Backend Type.
  std::string backendName;
  /// A string contains the node names(e.g. Add, Div) which are separeted by
  /// ",". E.g. "Div,Add". In Partitioner, those nodes won't be supported in
  /// this backend.
  std::string nonSupportedNodes;
  /// A string contains the node names(e.g. Add, Div) which are separeted by
  /// ",". E.g. "Div,Add". In Partitioner, the complementary set of those nodes
  /// won't be supported in this backend.
  std::string supportedNodes;
  /// Available SRAM capacity in bytes.
  uint64_t sramCapacity;
  /// Peak compute on device in ops/second. Assumes all ops are in int8.
  /// TODO: distinguish between data types with different peak flops.
  float peakCompute;
  /// Peak memory bandwidth from DRAM on device in bytes/second.
  float peakDramBw;
  /// Peak memory bandwidth from SRAM on device in bytes/second.
  float peakSramBw;
  /// Peak ingress/egress PCI-E bandwidth from device in bytes/second.
  float peakPCIeBw;
};

/// Individual Node in the DAG for a given network. This contains all the
/// information needed to run the sub-network at inference time.
struct DAGNode {
  /// The children of this node, these are nodes that depend on the current
  /// node.
  std::vector<DAGNode *> children;
  /// Pointers to the parents of this node. This is used by the executor for
  /// determining if a given node has all dependencies met.
  std::vector<DAGNode *> parents;
  /// IDs of the deviceManagers that this network is assigned to.
  std::vector<DeviceIDTy> deviceIDs;
  /// Backend name for this network.
  std::string backendName;
  /// The logicalDevice is an output of the Partitioner to indicate that two
  /// networks should be assigned to the same device. Multiple logical devices
  /// indicates the network should be duplicated.
  std::vector<DeviceIDTy> logicalDevices;
  /// Index of the current deviceID in deviceIDs. This is used by the Executor
  /// when picking a device to request a network run.
  unsigned currentDeviceIdx{0};
  /// Name assigned to the sub-network, this is the id that will be passed to
  /// the DeviceManager when requesting a run of the network.
  std::string name;
  /// Runtime bundle containing all the symbol information for this network at
  /// runtime.
  std::unique_ptr<RuntimeBundle> runtimeBundle;

  /// Backend Hints object, this is populated by the Partitioner and is used
  /// to communicated hints to the compiler, like SRAM pinning and resource
  /// reservation.
  BackendHints backendHints{};

  /// Pointer to module the function came from. This is so the executor can
  /// access the associated PHs for the function that are stored in the Module.
  Module *module{nullptr};

  DeviceIDTy getNextDevice() {
    currentDeviceIdx++;
    return deviceIDs[currentDeviceIdx % deviceIDs.size()];
  }
};

/// This struct represents a DAG. The first element is the root of a DAG, and
/// the second one is a list of all rest nodes in this DAG.
using DAGNodePtr = std::unique_ptr<DAGNode>;
using DAGNodePtrVec = std::vector<std::unique_ptr<DAGNode>>;

struct DAG {
  /// This is a root node it does not map directly to a loaded function. It
  /// contains the name of the network, a list of children, and a reference to
  /// the Module the function came from.
  DAGNodePtr root;
  /// This is a vector of all the DAGNodes. Structure is encoded in the DAGNodes
  /// with pointers to parents and children.
  DAGNodePtrVec nodes;
};

/// This list contains all the created DAGNodes from the Partitioner. The
/// contained DAGNodes can only refer to the DAGNodes from the same DAGListTy.
using DAGListTy = std::vector<DAG>;

/// This is the base class for DeviceManager configurations. Any specific
/// device can extend this class to contain information to identify
/// and configure the device manager. Additionally it needs to set it's backend
/// member variable to it's correct Backend.
struct DeviceConfig {
  /// Backend used for this config. It is used in
  /// checking the type of config before casting to a derived class.
  const std::string backendName;
  /// A human readable name to identify the device.
  std::string name;
  /// A runtime assigned id for the device. This is used for stats reporting.
  unsigned deviceID{0};
  /// Device memory size in bytes.
  uint64_t deviceMemory = 0;
  /// A map of configuration parameters.
  llvm::StringMap<std::string> parameters{};

  DeviceConfig(llvm::StringRef backendName) : backendName(backendName) {}
  DeviceConfig(llvm::StringRef backendName, llvm::StringRef name)
      : backendName(backendName), name(name) {}

  DeviceConfig(llvm::StringRef backendName, llvm::StringRef name,
               llvm::StringMap<std::string> parameters)
      : backendName(backendName), name(name), parameters(parameters) {}

  bool hasName() const { return name != ""; }

  void setDeviceMemory(uint64_t memSize) { deviceMemory = memSize; }

  uint64_t getDeviceMemory() const { return deviceMemory; }

  uint64_t getDeviceMemory(uint64_t defaultMemory) const {
    return deviceMemory == 0 ? defaultMemory : deviceMemory;
  }
};

/// Options configuring Host components of the Runtime, such as the Partitioner
/// and Executor.
struct HostConfig {
  /// Number of outstanding or concurrent networks before queueing.
  size_t maxActiveRequests{10};
  /// Number of requests to queue up before refusing further requests.
  size_t maxQueueSize{100};
  /// Number of threads to allocate to the Executor.
  size_t executorThreads{3};
};

/// This is struct for user defined partition.
struct PartitionConfig {
  /// The name of the function to be partitioned.
  std::string funcName;
  /// The number of user defined partitions.
  /// The partition ids are between 0 and numOfPartitions - 1, inclusive.
  size_t numOfPartitions;
  /// The backend for each partition. backendNames.size() == numOfPartitions.
  std::vector<std::string> backendNames;
  /// The name for each partition. partitionNames.size() == numOfPartitions.
  std::vector<std::string> partitionNames;
  /// The mapping between nodes' name to Partition ids. Assume there are n nodes
  /// and m partitions. We have 2 types of valid mapping: 1. all nodes are
  /// mapped to a partition. 2. For i-th (0 <= i < m) partition, the nodes
  /// mapped to this partition id are not in this map, and the nodes mapped to
  /// other partitions ids must be in this map. The node's name should be the
  /// name in Glow function and may be different from the original name from
  /// models. Since Glow will mangle names to make them unique.
  llvm::StringMap<size_t> nodeToPartition;

  PartitionConfig() : numOfPartitions(0) {}
  bool enabled() { return numOfPartitions > 0; }
};

} // namespace runtime
} // namespace glow
#endif // GLOW_RUNTIME_RUNTIMETYPES_H
