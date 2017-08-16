/// @author    Johannes de Fine Licht (johannes.definelicht@inf.ethz.ch)
/// @date      August 2017 
/// @copyright This software is copyrighted under the BSD 3-Clause License. 

#include "MatrixMatrix.h"
#include "hlslib/Stream.h"
#include "hlslib/Simulation.h"
#include <sstream>
#include <iostream>

inline int GlobalIndex(int bn, int bp, int tn, int tp) {
  #pragma HLS INLINE
  return (bn * kTileSizeN + tn) * kSize + bp * kTileSizeP + tp;
}

inline int GlobalIndexKernel(int bn, int bp, int tn, int tp) {
  #pragma HLS INLINE
  return (bn * kTileSizeN + tn) * kSizeKernel + bp * kTileSizePKernel + tp;
}

inline int GlobalIndexMemory(int bn, int bp, int tn, int tp) {
  #pragma HLS INLINE
  return (bn * kTileSizeN + tn) * kSizeMemory + bp * kTileSizePMemory + tp;
}

enum class State {
  streaming,
  storingC
};

void MatrixMatrixStage(int id,
                       hlslib::Stream<Data_t> &aIn,
                       hlslib::Stream<KernelPack_t> &bIn,
                       hlslib::Stream<KernelPack_t> &cIn,
                       hlslib::Stream<Data_t> &aOut,
                       hlslib::Stream<KernelPack_t> &bOut,
                       hlslib::Stream<KernelPack_t> &cOut) {

  int i_loadA_tn = 0;
  const int i_loadA_tn_end = kTileSizeN - id;
  int i_streamB_tp = 0;
  const int i_streamB_tp_end = kTileSizePKernel;
  int i_outer = 0;
  const int i_outer_end = kSize;
  int i_storeC = 0;
  const int i_storeC_end = (kTileSizeN - id) * kTileSizePKernel;
  const int i_saturated_end = kTileSizeN - id; 

Blocks_N:
  for (int bn = 0; bn < kBlocksN; ++bn) {
  Blocks_P:
    for (int bp = 0; bp < kBlocksP; ++bp) {

// #ifndef MM_SYNTHESIS
//       hlslib::Stream<KernelPack_t> cLocal("cLocal");
// #else
//       hls::stream<KernelPack_t> cLocal("cLocal");
//       #pragma HLS STREAM variable=cLocal depth=kTileSizePKernel
// #endif
      KernelPack_t cLocal[kTileSizePKernel];

      Data_t aNext, aVal;

      // Manually flattened loop
      const int loopBound =
          i_loadA_tn_end + kSize * i_streamB_tp_end + i_storeC_end;
    Flattened:
      for (int i = 0; i < loopBound; ++i) {
        #pragma HLS LOOP_FLATTEN
        #pragma HLS PIPELINE

        if (i < loopBound - i_storeC_end) {

          // Grab next from previous iteration. This way we avoid that the
          // last processing elements overwrites its next value before it is
          // used
          if (i_streamB_tp == 0) {
            aVal = aNext;
          }
          const bool loadA =
              (i_streamB_tp < i_loadA_tn_end) && (i_outer < i_outer_end - 1);
          if (loadA) {
            const auto aRead = hlslib::ReadBlocking(aIn);
            // Don't forward on the last iteration
            if (i_loadA_tn == 0) {
              aNext = aRead;
            } else {
              hlslib::WriteBlocking(aOut, aRead, 1);
            }
            if (i_loadA_tn == i_loadA_tn_end - 1) {
              i_loadA_tn = 0;
            } else {
              ++i_loadA_tn;
            }
          }
          if (i < i_saturated_end) {
            continue;
          }
          const auto readB = hlslib::ReadBlocking(bIn); 
          if (id < kTileSizeN - 1) {
            hlslib::WriteBlocking(bOut, readB, 1); // Forward B
          }
          KernelPack_t cAcc;
          if (i_outer > 0) {
            cAcc = cLocal[i_streamB_tp];
            #pragma HLS DEPENDENCE variable=cLocal inter false
            // cAcc = hlslib::ReadOptimistic(cLocal);
          } else {
            cAcc = KernelPack_t(OperatorReduce::identity());
          }
          KernelPack_t result;
        UnrollVector:
          for (int w = 0; w < kKernelWidth; ++w) {
            #pragma HLS UNROLL
            const auto map = OperatorMap::Apply(readB[w], aVal);
            result[w] = OperatorReduce::Apply(map, cAcc[w]);
          }
          // hlslib::WriteOptimistic(cLocal, result, kTileSizePKernel);
          cLocal[i_streamB_tp] = result;
          #pragma HLS DEPENDENCE variable=cLocal inter false
          if (i_streamB_tp == i_streamB_tp_end - 1) {
            i_streamB_tp = 0;
            if (i_outer == i_outer_end - 1) {
              i_outer = 0;
            } else {
              ++i_outer;
            }
          } else {
            ++i_streamB_tp;
          }

        } else {

          if (i_storeC < kTileSizePKernel) {
            // hlslib::WriteBlocking(cOut, hlslib::ReadOptimistic(cLocal), 1);
            hlslib::WriteBlocking(cOut, cLocal[i_storeC], 1);
            #pragma HLS DEPENDENCE variable=cLocal inter false
          } else {
            hlslib::WriteBlocking(cOut, hlslib::ReadBlocking(cIn), 1);
          }
          if (i_storeC == i_storeC_end - 1) {
            i_storeC = 0;
          } else {
            ++i_storeC;
          }

        }

      }

    }
  }
}

void ReadA(Data_t const a[], hlslib::Stream<Data_t> &aPipe) {
ReadA_Block_N:
  for (int bn = 0; bn < kBlocksN; ++bn) {
  ReadA_Block_P:
    for (int bp = 0; bp < kBlocksP; ++bp) {
    ReadA_M:
      for (int m = 0; m < kSize; ++m) {
      ReadA_N:
        for (int tn = 0; tn < kTileSizeN; ++tn) {
          #pragma HLS LOOP_FLATTEN
          #pragma HLS PIPELINE
          hlslib::WriteBlocking(aPipe, a[GlobalIndex(bn, 0, tn, m)], 1);
        }
      }
    }
  }
}

void ReadBMemory(MemoryPack_t const b[], hlslib::Stream<MemoryPack_t> &bPipe) {
ReadB_Block_N:
  for (int bn = 0; bn < kBlocksN; ++bn) {
  ReadB_Block_P:
    for (int bp = 0; bp < kBlocksP; ++bp) {
    ReadB_M:
      for (int m = 0; m < kSize; ++m) {
      ReadB_P:
        for (int tp = 0; tp < kTileSizePMemory; ++tp) {
          #pragma HLS LOOP_FLATTEN
          #pragma HLS PIPELINE
          hlslib::WriteBlocking(bPipe, b[GlobalIndexMemory(0, bp, m, tp)], 1);
        }
      }
    }
  }

}

void ReadBKernel(hlslib::Stream<MemoryPack_t> &bMem,
                 hlslib::Stream<KernelPack_t> &bPipe) {
ReadB_Block_N:
  for (int bn = 0; bn < kBlocksN; ++bn) {
  ReadB_Block_P:
    for (int bp = 0; bp < kBlocksP; ++bp) {
    ReadB_M:
      for (int m = 0; m < kSize; ++m) {
      ReadB_P_Memory:
        for (int tpm = 0; tpm < kTileSizePMemory; ++tpm) {
          MemoryPack_t mem;
        ReadB_P_KernelPerMemory:
          for (int kpm = 0; kpm < kKernelPerMemory; ++kpm) { 
            #pragma HLS LOOP_FLATTEN
            #pragma HLS PIPELINE
            if (kpm == 0) {
              mem = hlslib::ReadBlocking(bMem);
            }
            const KernelPack_t kernel = mem[kpm];
            hlslib::WriteBlocking(bPipe, kernel, 1);
          }
        }
      }
    }
  }
}

void WriteCKernel(hlslib::Stream<KernelPack_t> &cPipe,
                  hlslib::Stream<MemoryPack_t> &cMem) {
WriteCKernel_Block_N:
  for (int bn = 0; bn < kBlocksN; ++bn) {
  WriteCKernel_Block_P:
    for (int bp = 0; bp < kBlocksP; ++bp) {
    WriteCKernel_N:
      for (int tn = 0; tn < kTileSizeN; ++tn) {
      WriteCKernel_P_Memory:
        for (int tp = 0; tp < kTileSizePMemory; ++tp) {
          MemoryPack_t mem;
        WriteCKernel_P_Kernel:
          for (int kpm = 0; kpm < kKernelPerMemory; ++kpm) {
            #pragma HLS LOOP_FLATTEN
            #pragma HLS PIPELINE
            const auto read = hlslib::ReadBlocking(cPipe);
            mem[kpm] = read;
            if (kpm == kKernelPerMemory - 1) {
              hlslib::WriteBlocking(cMem, mem, 1);
            }
          }
        }
      }
    }
  }
}

void WriteCMemory(hlslib::Stream<MemoryPack_t> &cMem, MemoryPack_t c[]) {
WriteC_Block_N:
  for (int bn = 0; bn < kBlocksN; ++bn) {
  WriteC_Block_P:
    for (int bp = 0; bp < kBlocksP; ++bp) {
    WriteC_N:
      for (int tn = 0; tn < kTileSizeN; ++tn) {
      WriteC_P:
        for (int tp = 0; tp < kTileSizePMemory; ++tp) {
          #pragma HLS LOOP_FLATTEN
          #pragma HLS PIPELINE
          c[GlobalIndexMemory(bn, bp, tn, tp)] = hlslib::ReadBlocking(cMem);
        }
      }
    }
  }
}

void MatrixMatrix(Data_t const a[], MemoryPack_t const b[], MemoryPack_t c[]) {

  #pragma HLS INTERFACE m_axi port=a offset=slave bundle=gmem0
  #pragma HLS INTERFACE m_axi port=b offset=slave bundle=gmem1
  #pragma HLS INTERFACE m_axi port=c offset=slave bundle=gmem2
  #pragma HLS INTERFACE s_axilite port=a bundle=control
  #pragma HLS INTERFACE s_axilite port=b bundle=control
  #pragma HLS INTERFACE s_axilite port=c bundle=control
  #pragma HLS INTERFACE s_axilite port=return bundle=control
  
  #pragma HLS DATAFLOW

  hlslib::Stream<Data_t> aPipes[kTileSizeN + 1];
  hlslib::Stream<MemoryPack_t> bMem("bMem");
  hlslib::Stream<KernelPack_t> bPipes[kTileSizeN + 1];
  hlslib::Stream<KernelPack_t> cPipes[kTileSizeN + 1];
  hlslib::Stream<MemoryPack_t> cMem("cMem");

  HLSLIB_DATAFLOW_INIT();

  HLSLIB_DATAFLOW_FUNCTION(ReadA, a, aPipes[0]);
  HLSLIB_DATAFLOW_FUNCTION(ReadBMemory, b, bMem);
  HLSLIB_DATAFLOW_FUNCTION(ReadBKernel, bMem, bPipes[0]);

#ifdef MM_SYNTHESIS
UnrollCompute:
  for (int tn = 0; tn < kTileSizeN; ++tn) {
    #pragma HLS UNROLL
    HLSLIB_DATAFLOW_FUNCTION(MatrixMatrixStage, tn, aPipes[tn], bPipes[tn],
                             cPipes[tn + 1], aPipes[tn + 1], bPipes[tn + 1],
                             cPipes[tn]);
  }
#else
  int arr[kTileSizeN];
  for (int tn = 0; tn < kTileSizeN; ++tn) {
    #pragma HLS UNROLL
    arr[tn] = tn; // Need to allow passing by value
    aPipes[tn].set_name("aPipes[" + std::to_string(tn) + "]");
    bPipes[tn].set_name("bPipes[" + std::to_string(tn) + "]");
    cPipes[tn].set_name("cPipes[" + std::to_string(tn) + "]");
    HLSLIB_DATAFLOW_FUNCTION(MatrixMatrixStage, arr[tn], aPipes[tn], bPipes[tn],
                             cPipes[tn + 1], aPipes[tn + 1], bPipes[tn + 1],
                             cPipes[tn]);
  }
  aPipes[kTileSizeN].set_name("aPipes[" + std::to_string(kTileSizeN) + "]");
  bPipes[kTileSizeN].set_name("bPipes[" + std::to_string(kTileSizeN) + "]");
  cPipes[kTileSizeN].set_name("cPipes[" + std::to_string(kTileSizeN) + "]");
#endif

  HLSLIB_DATAFLOW_FUNCTION(WriteCKernel, cPipes[0], cMem);
  HLSLIB_DATAFLOW_FUNCTION(WriteCMemory, cMem, c);

  HLSLIB_DATAFLOW_FINALIZE();
}
