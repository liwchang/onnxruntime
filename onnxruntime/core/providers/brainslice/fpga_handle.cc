#include "core/providers/brainslice/fpga_handle.h"
#include <fstream>
#include "load_firmware.h"
namespace onnxruntime {
namespace fpga {

template <typename T>
std::vector<T> LoadFile(const std::string& fileName) {
  std::vector<T> buf;
  std::ifstream file(fileName, std::ios::binary);

  if (file.good()) {
    file.exceptions(std::ifstream::failbit | std::ifstream::badbit | std::ifstream::eofbit);
  } else {
    throw std::runtime_error("Unable  to  open  file:  " + fileName);
  }

  file.seekg(0, std::ios::end);
  buf.resize(file.tellg() / sizeof(T));
  file.seekg(0, std::ios::beg);
  file.read(static_cast<char*>(static_cast<void*>(buf.data())), buf.size() * sizeof(T));
  return std::move(buf);
}

FPGAHandle::FPGAHandle(FPGAInfo info) : ip_(info.ip) {
  auto& fpga = FPGAUtil::Instance();
  if (info.need_configure) {
    ONNXRUNTIME_ENFORCE(LoadFirmware(info.inst_file, info.data_file, info.schema_file).IsOK());
  }
  ONNXRUNTIME_ENFORCE(fpga.GetCapacities(info.ip, &capacities_).IsOK());
  //TODO:  get  max  buffer  size
  max_request_size_ = fpga.GetBufferSize();
}

Status FPGAHandle::LoadMatrix(const std::vector<half_float::half>& matrix, const int rows, const int cols,
                              const int matix_addr, const bool row_major, const ISA_Mem mem_type) const {
  ONNXRUNTIME_ENFORCE((rows > 0) && (cols > 0) && (matrix.size() == rows * cols));

  //  To  keep  variable  naming  concise,  this  function  uses  "block"  to  refer  to  native  BrainSlice  matrices.
  //  Outside  of  this  comment,  "matrix"  generally  refers  to  the  input  matrix.
  int block_dim = capacities_.m_bsParameters.HWVEC_ELEMS;
  int bytes_per_block = block_dim * block_dim * sizeof(half_float::half);

  //  Subtract  message  header  overhead.  1KB  is  a  conservative  estimate.
  size_t max_request_bytes = max_request_size_;
  ONNXRUNTIME_ENFORCE(max_request_bytes > msg_header_bytes);
  max_request_bytes -= msg_header_bytes;
  ONNXRUNTIME_ENFORCE(max_request_bytes >= bytes_per_block);
  size_t max_blocks_per_pequest = max_request_bytes / bytes_per_block;

  //  Calculate  dimensions  of  input  matrix  in  terms  of  blocks
  int num_block_rows = (rows + block_dim - 1) / block_dim;
  int num_block_cols = (cols + block_dim - 1) / block_dim;
  int num_block_total = num_block_rows * num_block_cols;

  //  Basically  we  are  interleaving  this  doubly  nested  loop  in  with  the  per-request  while  loop,  iterating  over  the
  //  sub-blocks  of  the  input  matrix  in  row-major  order:
  //          for  (int  block_row_idx  =  0;  block_row_idx  <  num_block_rows;  ++block_row_idx)
  //                  for  (int  block_col_idx  =  0;  block_col_idx  <  num_block_cols;  ++block_col_idx)
  int block_row_idx = 0;
  int block_col_idx = 0;

  BS_TensorLoadParams args;

  //  Track  the  next  MRF  index.
  args.startAddr = matix_addr;
  args.memType = (ISA_Mem)mem_type;

  //  This  loop  generates  LoadMatrix  requests  until  there  are  no  more  blocks  left  to  load.
  size_t num_blocks_left = static_cast<size_t>(num_block_total);
  while (num_blocks_left > 0) {
    args.numTiles = static_cast<uint32_t>(std::min(max_blocks_per_pequest, num_blocks_left));

    ONNXRUNTIME_RETURN_IF_ERROR(SendSync([&](void* request, size_t* requestSize) {
                                                                          size_t  payloadBytes  =  args.numTiles  *  bytes_per_block;
                                                                          half_float::half*  payload;

                                                                          auto  status  =  BS_CommonFunctions_LoadMatrix_Request(
                                                                                  &capacities_.m_bsParameters,
                                                                                  &args,
                                                                                  (void**)&payload,  &payloadBytes,
                                                                                  request,  requestSize);

                                                                          if  (status)
                                                                              return  status;
                                                                          //  Copy  data.
                                                                          int  payloadIndex  =  0;
                                                                          for  (uint32_t  numCopiedBlocks  =  0;  numCopiedBlocks  <  args.numTiles;  ++numCopiedBlocks)  {
                                                                              //  Calculate  bounds  for  the  block  we  want  to  copy  in  terms  of  input  matrix  indices
                                                                              int  startingInputRowIdx  =  block_row_idx  *  block_dim;
                                                                              int  limitInputRowIdx  =  startingInputRowIdx  +  block_dim;
                                                                              int  startingInputColIdx  =  block_col_idx  *  block_dim;
                                                                              int  limitInputColIdx  =  startingInputColIdx  +  block_dim;

                                                                              //  Copy  next  block.
                                                                              for  (int  inputRowIdx  =  startingInputRowIdx;  inputRowIdx  <  limitInputRowIdx;  ++inputRowIdx)  {
                                                                                  //  Depending  on  how  good  the  compiler  is,  we  could  probably  speed  up  the  row-major  input  case  by  factoring  out  the  bounds  check
                                                                                  //  and  replacing  this  loop  with  a  memcpy().  We  should  investigate  if  this  becomes  performance  critical.
                                                                                  for  (int  inputColIdx  =  startingInputColIdx;  inputColIdx  <  limitInputColIdx;  ++inputColIdx)  {
                                                                                      half_float::half  val;

                                                                                      if  (inputRowIdx  <  rows  &&  inputColIdx  <  cols)  {
                                                                                          //  in  bounds,  read  value  from  input  matrix
                                                                                          int  idx  =  row_major  ?  (inputRowIdx  *  cols  +  inputColIdx)  :  (inputColIdx  *  rows  +  inputRowIdx);
                                                                                          ONNXRUNTIME_ENFORCE(idx  <  matrix.size());
                                                                                          val  =  matrix[idx];
                                                                                      }  else  {
                                                                                          //  out  of  bounds  of  input  matrix,  pad  with  zeros
                                                                                          val  =  0;
                                                                                      }
                                                                                      ONNXRUNTIME_ENFORCE(payloadIndex  *  sizeof(half_float::half)  <  payloadBytes);
                                                                                      payload[payloadIndex++]  =  val;
                                                                                  }
                                                                              }

                                                                              //  This  is  the  bottom  of  the  interleaved  for  loop  nest  mentioned  above.
                                                                              if  (++block_col_idx  >=  num_block_cols)  {
                                                                                  block_col_idx  =  0;
                                                                                  ++block_row_idx;
                                                                              }
                                                                          }

                                                                          return  status; },
                                         [&](const void* response, size_t responseSize) {
                                           return BS_CommonFunctions_LoadMatrix_Response(
                                               &capacities_.m_bsParameters,
                                               response, responseSize);
                                         }));

    //  Current  request  complete,  get  ready  for  next  one.
    num_blocks_left -= args.numTiles;
    args.startAddr += args.numTiles;
  }
  return Status::OK();
}

Status FPGAHandle::LoadVector(const std::vector<half_float::half>& vector, const int vec_addr, const ISA_Mem mem_type) const {
  //  Set  up  request  parameters
  int block_dim = capacities_.m_bsParameters.HWVEC_ELEMS;
  int bytes_per_block = block_dim * sizeof(half_float::half);

  //  Subtract  message  header  overhead.  1KB  is  a  conservative  estimate.
  size_t max_request_bytes = max_request_size_;
  ONNXRUNTIME_ENFORCE(max_request_bytes > msg_header_bytes);
  max_request_bytes -= msg_header_bytes;
  ONNXRUNTIME_ENFORCE(max_request_bytes >= bytes_per_block);
  size_t max_blocks_per_pequest = max_request_bytes / bytes_per_block;

  const int numBlocks = (static_cast<int>(vector.size()) + block_dim - 1) / block_dim;

  BS_TensorLoadParams args;
  args.memType = (ISA_Mem)mem_type;

  for (int currentBlock = 0; currentBlock < numBlocks; currentBlock += args.numTiles) {
    const int elementsAlreadyUploaded = currentBlock * block_dim;
    args.numTiles = static_cast<uint32_t>(std::min(max_blocks_per_pequest, static_cast<size_t>(numBlocks - currentBlock)));
    args.startAddr = vec_addr + currentBlock;

    size_t payloadBytes = args.numTiles * bytes_per_block;

    SendSync([&](void* request, size_t* requestSize) {
				half_float::half    *payload;

				auto  status  =  BS_CommonFunctions_LoadVector_Request(
					&capacities_.m_bsParameters,
					&args,
					(void**)&payload,  &payloadBytes,
					request,  requestSize);

				if  (status)
					return  status;

				//  copy  vector  and  pad  with  0
				memcpy(payload,  vector.data()  +  elementsAlreadyUploaded,  payloadBytes);

				const  std::size_t  elementsLeftInVector  =  vector.size()  -  elementsAlreadyUploaded;
				for  (auto  i  =  elementsLeftInVector;  i  <  args.numTiles  *  block_dim;  ++i)
				{
					payload[i]  =  half_float::half(0.0f);
				}

				return  status; },
             [&](const void* response, size_t responseSize) {
               return BS_CommonFunctions_LoadVector_Response(
                   &capacities_.m_bsParameters,
                   response, responseSize);
             });
  }
  return Status::OK();
}

Status FPGAHandle::SendSync(std::function<int32_t(void*, size_t*)> prepare_request, std::function<int32_t(void*, size_t)> process_response) const {
  auto& fpga = FPGAUtil::Instance();
  return fpga.SendSync(ip_, prepare_request, process_response);
}

Status FPGAHandle::LoadFirmware(const std::string& inst_file,
                                const std::string& data_file,
                                const std::string& schema_file) {
  return LoadFirmware(LoadFile<uint32_t>(inst_file),
                      LoadFile<uint32_t>(data_file),
                      LoadFile<uint64_t>(schema_file));
}

Status FPGAHandle::LoadFirmware(std::vector<uint32_t>&& inst,
                                std::vector<uint32_t>&& data,
                                std::vector<uint64_t>&& schema) {
  uint32_t* p_inst = !inst.empty() ? &inst[0] : nullptr;
  size_t n_inst = inst.size();
  uint32_t* p_data = !data.empty() ? &data[0] : nullptr;
  size_t n_data = data.size();
  uint64_t* p_schema = !schema.empty() ? &schema[0] : nullptr;
  size_t n_schema = schema.size();
  return SendSync([p_inst, n_inst, p_data, n_data, p_schema, n_schema](void* buffer, size_t* size) {
    return BrainSlice::LoadFirmwareAPI(p_inst,
                                       n_inst,
                                       p_data,
                                       n_data,
                                       p_schema,
                                       n_schema,
                                       buffer, size);
  },
                  nullptr);
}

}  //  namespace  fpga
}  //  namespace  onnxruntime
