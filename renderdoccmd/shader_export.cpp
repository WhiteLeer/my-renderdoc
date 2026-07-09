/******************************************************************************
 * The MIT License (MIT)
 ******************************************************************************/

#include "shader_export.h"

#include "../renderdoc/api/replay/pipestate.inl"
#include "capture_variant.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::string ToStd(const rdcstr &value)
{
  return std::string(value.begin(), value.end());
}

rdcstr FromStd(const std::string &value)
{
  return rdcstr(value.c_str(), value.size());
}

const char *ShaderStageName(ShaderStage stage)
{
  switch(stage)
  {
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::Hull: return "hull";
    case ShaderStage::Domain: return "domain";
    case ShaderStage::Geometry: return "geometry";
    case ShaderStage::Pixel: return "pixel";
    case ShaderStage::Compute: return "compute";
    case ShaderStage::Task: return "task";
    case ShaderStage::Mesh: return "mesh";
    case ShaderStage::RayGen: return "raygen";
    case ShaderStage::Intersection: return "intersection";
    case ShaderStage::AnyHit: return "anyhit";
    case ShaderStage::ClosestHit: return "closesthit";
    case ShaderStage::Miss: return "miss";
    case ShaderStage::Callable: return "callable";
    default: return "unknown";
  }
}

const char *ShaderEncodingName(ShaderEncoding encoding)
{
  switch(encoding)
  {
    case ShaderEncoding::DXBC: return "DXBC";
    case ShaderEncoding::GLSL: return "GLSL";
    case ShaderEncoding::SPIRV: return "SPIR-V";
    case ShaderEncoding::SPIRVAsm: return "SPIR-V ASM";
    case ShaderEncoding::HLSL: return "HLSL";
    case ShaderEncoding::DXIL: return "DXIL";
    case ShaderEncoding::OpenGLSPIRV: return "OpenGL SPIR-V";
    case ShaderEncoding::OpenGLSPIRVAsm: return "OpenGL SPIR-V ASM";
    case ShaderEncoding::Slang: return "Slang";
    default: return "Unknown";
  }
}

std::string ResourceIdName(ResourceId id)
{
  uint64_t numericId = 0;
  static_assert(sizeof(id) == sizeof(numericId), "ResourceId must remain 1:1 with uint64_t");
  memcpy(&numericId, &id, sizeof(numericId));
  return "ResourceId::" + std::to_string(numericId);
}

std::string JsonEscape(const std::string &value)
{
  std::ostringstream escaped;
  for(char c : value)
  {
    switch(c)
    {
      case '\\': escaped << "\\\\"; break;
      case '"': escaped << "\\\""; break;
      case '\n': escaped << "\\n"; break;
      case '\r': escaped << "\\r"; break;
      case '\t': escaped << "\\t"; break;
      default: escaped << (static_cast<unsigned char>(c) < 0x20 ? ' ' : c); break;
    }
  }
  return escaped.str();
}

std::string SafeName(std::string value)
{
  if(value.empty())
    return "unknown";
  for(char &c : value)
    if(!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
      c = '_';
  return value;
}

std::string JoinPath(const std::string &left, const std::string &right)
{
  if(left.empty())
    return right;
  if(left.back() == '\\' || left.back() == '/')
    return left + right;
  return left + "/" + right;
}

std::string ContentHash(const void *data, size_t size)
{
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  uint64_t state[4] = {0xcbf29ce484222325ULL, 0x84222325cbf29ce4ULL,
                       0x9e3779b185ebca87ULL, 0x517cc1b727220a95ULL};
  const uint64_t primes[4] = {0x100000001b3ULL, 0x100000001b3ULL, 0x100000001b3ULL,
                              0x100000001b3ULL};
  for(size_t i = 0; i < size; i++)
  {
    for(size_t lane = 0; lane < 4; lane++)
    {
      state[lane] ^= uint64_t(bytes[i]) + uint64_t(lane * 0x9d);
      state[lane] *= primes[lane];
      state[lane] ^= state[lane] >> 29;
    }
  }

  std::ostringstream hash;
  hash << std::hex << std::setfill('0');
  for(uint64_t lane : state)
    hash << std::setw(16) << lane;
  return hash.str();
}

std::string TextHash(const std::string &text)
{
  return ContentHash(text.data(), text.size());
}

bool EnsureParentDirectory(const std::string &filename)
{
  const size_t lastSeparator = filename.find_last_of("\\/");
  if(lastSeparator == std::string::npos)
    return true;

  const std::string parent = filename.substr(0, lastSeparator);
  std::string current;
  for(size_t i = 0; i < parent.size(); i++)
  {
    current.push_back(parent[i]);
    if(parent[i] != '\\' && parent[i] != '/')
      continue;

    if(current.size() <= 1 || (current.size() == 3 && current[1] == ':'))
      continue;
#if defined(_WIN32)
    if(_mkdir(current.c_str()) != 0 && errno != EEXIST)
#else
    if(mkdir(current.c_str(), 0777) != 0 && errno != EEXIST)
#endif
      return false;
  }

  if(!parent.empty())
  {
#if defined(_WIN32)
    if(_mkdir(parent.c_str()) != 0 && errno != EEXIST)
#else
    if(mkdir(parent.c_str(), 0777) != 0 && errno != EEXIST)
#endif
      return false;
  }
  return true;
}

bool WriteText(const std::string &filename, const std::string &text)
{
  if(!EnsureParentDirectory(filename))
    return false;
  std::ofstream file(filename, std::ios::binary);
  if(!file)
    return false;
  file.write(text.data(), (std::streamsize)text.size());
  return file.good();
}

bool WriteBinary(const std::string &filename, const void *data, size_t size)
{
  if(!EnsureParentDirectory(filename))
    return false;
  std::ofstream file(filename, std::ios::binary);
  if(!file)
    return false;
  file.write(static_cast<const char *>(data), (std::streamsize)size);
  return file.good();
}

void FlattenActions(const rdcarray<ActionDescription> &actions,
                    std::vector<const ActionDescription *> &flattened)
{
  for(const ActionDescription &action : actions)
  {
    flattened.push_back(&action);
    FlattenActions(action.children, flattened);
  }
}

bool IsShaderAction(const ActionDescription &action)
{
  return bool(action.flags & (ActionFlags::Drawcall | ActionFlags::Dispatch |
                              ActionFlags::MeshDispatch | ActionFlags::DispatchRay));
}

std::string ReflectionSummary(const ShaderReflection &reflection)
{
  std::ostringstream summary;
  summary << "stage=" << ShaderStageName(reflection.stage) << "\n";
  summary << "encoding=" << ShaderEncodingName(reflection.encoding) << "\n";
  summary << "entryPoint=" << ToStd(reflection.entryPoint) << "\n";
  summary << "constantBlocks=" << reflection.constantBlocks.size() << "\n";
  for(const ConstantBlock &block : reflection.constantBlocks)
  {
    summary << "  cbuffer " << ToStd(block.name) << " bind=" << block.fixedBindNumber
            << " set=" << block.fixedBindSetOrSpace << " bytes=" << block.byteSize << "\n";
    for(const ShaderConstant &constant : block.variables)
      summary << "    " << ToStd(constant.name) << " offset=" << constant.byteOffset << "\n";
  }
  summary << "samplers=" << reflection.samplers.size() << "\n";
  for(const ShaderSampler &sampler : reflection.samplers)
    summary << "  " << ToStd(sampler.name) << " bind=" << sampler.fixedBindNumber
            << " set=" << sampler.fixedBindSetOrSpace << "\n";
  summary << "readOnlyResources=" << reflection.readOnlyResources.size() << "\n";
  for(const ShaderResource &resource : reflection.readOnlyResources)
    summary << "  " << ToStd(resource.name) << " bind=" << resource.fixedBindNumber
            << " set=" << resource.fixedBindSetOrSpace << "\n";
  summary << "readWriteResources=" << reflection.readWriteResources.size() << "\n";
  for(const ShaderResource &resource : reflection.readWriteResources)
    summary << "  " << ToStd(resource.name) << " bind=" << resource.fixedBindNumber
            << " set=" << resource.fixedBindSetOrSpace << "\n";
  return summary.str();
}

std::string ShaderLabTemplate(uint32_t eventId, ShaderStage stage, const std::string &entryPoint,
                              const std::string &sourceFile, const std::string &target,
                              const std::string &disassembly)
{
  const std::string stageName = ShaderStageName(stage);

  std::ostringstream shader;
  shader << "// Generated from RenderDoc capture event " << eventId << " (" << stageName
         << ").\n";
  shader << "// Reconstruction template: backend source is preserved, but Unity stage linkage and "
            "bindings may require adaptation.\n";
  shader << "// Backend target: " << target << "\n";
  shader << "// Decompiled source: " << sourceFile << "\n\n";
  shader << "Shader \"RenderDoc/Reconstructed/Event_" << eventId << "_" << SafeName(stageName)
         << "\"\n{\n    SubShader\n    {\n        Pass\n        {\n            "
          << "HLSLPROGRAM\n";
  shader << "            // Original entry point: " << entryPoint << "\n";
  shader << "            #pragma vertex RenderDocTemplateVertex\n"
            "            #pragma fragment RenderDocTemplateFragment\n"
            "            struct RenderDocTemplateAttributes { float4 vertex : POSITION; };\n"
            "            struct RenderDocTemplateVaryings { float4 vertex : SV_POSITION; };\n"
            "            RenderDocTemplateVaryings RenderDocTemplateVertex(RenderDocTemplateAttributes input)\n"
            "            { RenderDocTemplateVaryings output; output.vertex = input.vertex; return output; }\n"
            "            half4 RenderDocTemplateFragment() : SV_Target { return half4(1, 0, 1, 1); }\n";
  shader << "            /*\n" << disassembly << "\n            */\n";
  shader << "            ENDHLSL\n        }\n    }\n}\n";
  return shader.str();
}

class ShaderExportCommand : public Command
{
private:
  std::string filename;
  std::string output;
  std::string target;
  uint32_t eventId = 0;
  bool listTargets = false;

public:
  void AddOptions(cmdline::parser &parser) override
  {
    parser.set_footer("<capture.rdc|capture.srrdc|capture.zzzrdc>");
    parser.add<std::string>("output", 'o', "Output directory.", true);
    parser.add<uint32_t>("event", 'e', "Export only this event ID; 0 exports all shader actions.",
                         false, 0);
    parser.add<std::string>("target", 't',
                            "RenderDoc disassembly target; empty uses the backend default.", false,
                            "");
    parser.add("list-targets", 'l', "List disassembly targets available for this capture and exit.");
  }

  const char *Description() override
  {
    return "Export shader reflection, backend disassembly and ShaderLab reconstruction templates.";
  }

  bool IsInternalOnly() override { return false; }
  bool IsCaptureCommand() override { return false; }

  bool Parse(cmdline::parser &parser, GlobalEnvironment &) override
  {
    std::vector<std::string> rest = parser.rest();
    if(rest.empty())
    {
      std::cerr << "Error: shader-export requires a capture filename." << std::endl
                << std::endl
                << parser.usage();
      return false;
    }
    filename = rest[0];
    output = parser.get<std::string>("output");
    target = parser.get<std::string>("target");
    eventId = parser.get<uint32_t>("event");
    listTargets = parser.exist("list-targets");
    return true;
  }

  int Execute(const CaptureOptions &) override
  {
    ICaptureFile *file = RENDERDOC_OpenCaptureFile();
    ResultDetails opened = file->OpenFile(FromStd(filename), RENDERDOC_CAPTURE_FILE_EXTENSION, NULL);
    if(!opened.OK())
    {
      std::cerr << "Couldn't open '" << filename << "': " << ToStd(opened.Message()) << std::endl;
      file->Shutdown();
      return 1;
    }

    ResultDetails replayResult = {};
    IReplayController *controller = NULL;
    rdctie(replayResult, controller) = file->OpenCapture(ReplayOptions(), NULL);
    file->Shutdown();
    if(!replayResult.OK() || controller == NULL)
    {
      std::cerr << "Couldn't replay '" << filename << "': " << ToStd(replayResult.Message())
                << std::endl;
      return 1;
    }

    if(listTargets)
    {
      for(const rdcstr &availableTarget : controller->GetDisassemblyTargets(true))
        std::cout << ToStd(availableTarget) << std::endl;
      controller->Shutdown();
      return 0;
    }

    std::ostringstream shaderIndex;
    std::ostringstream variantIndex;
    std::ostringstream eventIndex;
    shaderIndex << "  \"shaders\": [\n";
    variantIndex << "  \"variants\": [\n";
    eventIndex << "  \"events\": [\n";

    std::vector<const ActionDescription *> actions;
    FlattenActions(controller->GetRootActions(), actions);
    std::set<std::string> exportedShaders;
    std::set<std::string> exportedVariants;
    bool wroteShader = false;
    bool wroteVariant = false;
    bool wroteEvent = false;
    uint32_t exportedStages = 0;
    uint32_t exportedEvents = 0;

    for(const ActionDescription *action : actions)
    {
      if(action == NULL || !IsShaderAction(*action) || (eventId != 0 && action->eventId != eventId))
        continue;

      controller->SetFrameEvent(action->eventId, true);
      const PipeState &pipe = controller->GetPipelineState();
      const ResourceId graphicsPipeline = pipe.GetGraphicsPipelineObject();
      const ResourceId computePipeline = pipe.GetComputePipelineObject();
      const ResourceId pipeline = graphicsPipeline != ResourceId() ? graphicsPipeline : computePipeline;

      std::ostringstream eventJson;
      eventJson << "    {\n      \"eventId\": " << action->eventId << ",\n";
      eventJson << "      \"name\": \"" << JsonEscape(ToStd(action->customName))
                << "\",\n      \"stages\": [\n";
      bool wroteStage = false;

      for(uint32_t stageIndex = 0; stageIndex < (uint32_t)ShaderStage::Count; stageIndex++)
      {
        const ShaderStage stage = (ShaderStage)stageIndex;
        const ResourceId shader = pipe.GetShader(stage);
        if(shader == ResourceId())
          continue;

        rdcarray<ShaderEntryPoint> entryPoints = controller->GetShaderEntryPoints(shader);
        ShaderEntryPoint entry = entryPoints.empty() ? ShaderEntryPoint("", stage) : entryPoints[0];
        const rdcstr pipelineEntry = pipe.GetShaderEntryPoint(stage);
        if(!pipelineEntry.empty())
          entry.name = pipelineEntry;
        entry.stage = stage;

        const ShaderReflection *reflection = controller->GetShader(pipeline, shader, entry);
        if(reflection == NULL)
          continue;

        const rdcstr decompiled = controller->DisassembleShader(pipeline, reflection, FromStd(target));
        const std::string stageName = SafeName(ShaderStageName(stage));
        const std::string reflectionText = ReflectionSummary(*reflection);
        const size_t rawByteCount = reflection->rawBytes.count();
        const std::string shaderHash = rawByteCount > 0
                                           ? ContentHash(reflection->rawBytes.data(), rawByteCount)
                                           : "no-bytes-" + ResourceIdName(shader);
        const std::string interfaceHash = ContentHash(reflectionText.data(), reflectionText.size());
        const std::string shaderRecordKey = shaderHash + "|" + stageName + "|" + interfaceHash;
        const std::string shaderStem = "shader_" + shaderHash + "_" + stageName + "_" +
                                       interfaceHash.substr(0, 16);
        const bool hlslTarget = target.find("HLSL") != std::string::npos ||
                                target.find("hlsl") != std::string::npos;
        const std::string shaderFile = "shaders/" + shaderStem +
                                       (hlslTarget ? ".hlsl" : ".disasm.txt");
        const std::string binaryFile = "shaders/" + shaderStem + ".bin";
        const std::string reflectionFile = "shaders/" + shaderStem + ".reflection.txt";
        const std::string shaderLabFile = "shaders/" + shaderStem + ".shader";
        const std::string decompiledText = ToStd(decompiled);
        if(exportedShaders.insert(shaderRecordKey).second)
        {
          if((rawByteCount > 0 &&
              !WriteBinary(JoinPath(output, binaryFile), reflection->rawBytes.data(), rawByteCount)) ||
             (rawByteCount == 0 && !WriteText(JoinPath(output, binaryFile), "No raw shader bytes.")) ||
             !WriteText(JoinPath(output, shaderFile), decompiledText) ||
             !WriteText(JoinPath(output, reflectionFile), reflectionText) ||
             !WriteText(JoinPath(output, shaderLabFile),
                        ShaderLabTemplate(action->eventId, stage, ToStd(entry.name), shaderFile,
                                          target, decompiledText)))
          {
            std::cerr << "Couldn't write shader export for event " << action->eventId << std::endl;
            controller->Shutdown();
            return 1;
          }

          if(wroteShader)
            shaderIndex << ",\n";
          shaderIndex << "    {\n      \"shaderHash\": \"" << shaderHash
                      << "\",\n      \"interfaceHash\": \"" << interfaceHash
                      << "\",\n      \"stage\": \"" << stageName
                      << "\",\n      \"resourceId\": \"" << ResourceIdName(shader)
                      << "\",\n      \"rawByteSize\": " << rawByteCount
                      << ",\n      \"source\": \"" << shaderFile
                      << "\",\n      \"binary\": \"" << binaryFile
                      << "\",\n      \"reflection\": \"" << reflectionFile
                      << "\",\n      \"shaderLab\": \"" << shaderLabFile << "\"\n    }";
          wroteShader = true;
        }

        const std::string entryPoint = ToStd(entry.name);
        const std::string pipelineId = ResourceIdName(pipeline);
        const std::string variantMaterial = shaderRecordKey + "|entry=" + entryPoint +
                                            "|pipeline=" + pipelineId;
        const std::string variantHash = TextHash(variantMaterial);
        const std::string variantFile = "variants/variant_" + variantHash + ".json";
        if(exportedVariants.insert(variantHash).second)
        {
          std::ostringstream variantJson;
          variantJson << "{\n      \"variantHash\": \"" << variantHash
                      << "\",\n      \"shaderHash\": \"" << shaderHash
                      << "\",\n      \"interfaceHash\": \"" << interfaceHash
                      << "\",\n      \"stage\": \"" << stageName
                      << "\",\n      \"entryPoint\": \"" << JsonEscape(entryPoint)
                      << "\",\n      \"pipelineId\": \"" << pipelineId
                      << "\",\n      \"specialization\": \"included in pipeline identity; raw values require API-specific export\"\n    }\n";
          if(!WriteText(JoinPath(output, variantFile), variantJson.str()))
          {
            std::cerr << "Couldn't write variant export for event " << action->eventId
                      << std::endl;
            controller->Shutdown();
            return 1;
          }
          if(wroteVariant)
            variantIndex << ",\n";
          variantIndex << "    {\n      \"variantHash\": \"" << variantHash
                      << "\",\n      \"shaderHash\": \"" << shaderHash
                      << "\",\n      \"interfaceHash\": \"" << interfaceHash
                      << "\",\n      \"stage\": \"" << stageName
                      << "\",\n      \"entryPoint\": \"" << JsonEscape(entryPoint)
                      << "\",\n      \"pipelineId\": \"" << pipelineId
                      << "\",\n      \"specialization\": \"included in pipeline identity; raw values require API-specific export\"\n    }";
          wroteVariant = true;
        }

        if(wroteStage)
          eventJson << ",\n";
        eventJson << "        {\n          \"stage\": \""
                  << JsonEscape(ShaderStageName(stage)) << "\",\n";
        eventJson << "          \"resourceId\": \""
                  << JsonEscape(ResourceIdName(shader))
                  << "\",\n          \"shaderHash\": \"" << shaderHash
                  << "\",\n          \"interfaceHash\": \"" << interfaceHash
                  << "\",\n          \"variantHash\": \"" << variantHash
                  << "\",\n          \"encoding\": \""
                  << JsonEscape(ShaderEncodingName(reflection->encoding)) << "\",\n";
        eventJson << "          \"entryPoint\": \"" << JsonEscape(entryPoint)
                  << "\",\n          \"pipelineId\": \"" << pipelineId
                  << "\",\n          \"source\": \"" << shaderFile
                  << "\",\n          \"reflection\": \"" << reflectionFile
                  << "\",\n          \"shaderLab\": \"" << shaderLabFile << "\"\n        }";
        wroteStage = true;
        exportedStages++;
      }

      if(!wroteStage)
        continue;

      eventJson << "\n      ],\n      \"bindings\": [";
      const rdcarray<UsedDescriptor> bindings = pipe.GetAllUsedDescriptors(false);
      for(size_t bindingIndex = 0; bindingIndex < bindings.size(); bindingIndex++)
      {
        const UsedDescriptor &binding = bindings[bindingIndex];
        if(bindingIndex > 0)
          eventJson << ",";
        eventJson << "\n        {\n          \"stage\": \""
                  << JsonEscape(ShaderStageName(binding.access.stage))
                  << "\",\n          \"index\": " << binding.access.index
                  << ",\n          \"arrayElement\": " << binding.access.arrayElement
                  << ",\n          \"type\": " << (uint32_t)binding.access.type
                  << ",\n          \"resource\": \"" << ResourceIdName(binding.descriptor.resource)
                  << "\",\n          \"secondary\": \"" << ResourceIdName(binding.descriptor.secondary)
                  << "\",\n          \"view\": \"" << ResourceIdName(binding.descriptor.view)
                  << "\",\n          \"byteOffset\": " << binding.descriptor.byteOffset
                  << ",\n          \"byteSize\": " << binding.descriptor.byteSize << "\n        }";
      }
      eventJson << "\n      ]\n    }";
      if(wroteEvent)
        eventIndex << ",\n";
      eventIndex << eventJson.str();
      wroteEvent = true;
      exportedEvents++;
      if(eventId != 0)
        break;
    }

    shaderIndex << "\n  ]";
    variantIndex << "\n  ]";
    eventIndex << "\n  ]";
    std::ostringstream manifest;
    manifest << "{\n  \"format\": \"RenderDocShaderExport/2\",\n"
             << "  \"capture\": \"" << JsonEscape(filename) << "\",\n"
             << "  \"target\": \"" << JsonEscape(target) << "\",\n"
             << "  \"summary\": {\n    \"events\": " << exportedEvents
             << ",\n    \"stageUses\": " << exportedStages
             << ",\n    \"uniqueShaders\": " << exportedShaders.size()
             << ",\n    \"uniqueVariants\": " << exportedVariants.size() << "\n  },\n"
             << shaderIndex.str() << ",\n" << variantIndex.str() << ",\n"
             << eventIndex.str() << "\n}\n";
    const bool manifestWritten = WriteText(JoinPath(output, "capture.json"), manifest.str());
    controller->Shutdown();
    if(!manifestWritten)
    {
      std::cerr << "Couldn't write shader export manifest." << std::endl;
      return 1;
    }

    std::cout << "Exported " << exportedEvents << " event(s), " << exportedStages
              << " stage use(s), " << exportedShaders.size() << " unique shader record(s), and "
              << exportedVariants.size() << " variant(s) to '" << output << "'."
              << std::endl;
    return 0;
  }
};
}    // namespace

Command *CreateShaderExportCommand()
{
  return new ShaderExportCommand();
}
