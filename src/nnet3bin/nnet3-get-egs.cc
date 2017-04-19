// nnet3bin/nnet3-get-egs.cc

// Copyright 2012-2015  Johns Hopkins University (author:  Daniel Povey)
//                2014  Vimal Manohar

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include <sstream>
#include "base/kaldi-common.h"
#include "util/common-utils.h"
#include "hmm/transition-model.h"
#include "hmm/posterior.h"
#include "nnet3/nnet-example.h"
#include "nnet3/nnet-example-utils.h"

namespace kaldi {
namespace nnet3 {


static bool ProcessFile(const MatrixBase<BaseFloat> &feats,
                        const MatrixBase<BaseFloat> *ivector_feats,
                        int32 ivector_period,
                        bool ivector_as_input,
                        bool ivector_as_output,
                        float32 ivector_scale_factor,
                        const Posterior &pdf_post,
                        const std::string &utt_id,
                        bool compress,
                        int32 num_pdfs,
                        UtteranceSplitter *utt_splitter,
                        NnetExampleWriter *example_writer) {
  int32 num_input_frames = feats.NumRows();
  if (!utt_splitter->LengthsMatch(utt_id, num_input_frames,
                             static_cast<int32>(pdf_post.size())))
    return false;  // LengthsMatch() will have printed a warning.

  std::vector<ChunkTimeInfo> chunks;

  utt_splitter->GetChunksForUtterance(num_input_frames, &chunks);

  if (chunks.empty()) {
    KALDI_WARN << "Not producing egs for utterance " << utt_id
               << " because it is too short: "
               << num_input_frames << " frames.";
  }

  // 'frame_subsampling_factor' is not used in any recipes at the time of
  // writing, this is being supported to unify the code with the 'chain' recipes
  // and in case we need it for some reason in future.
  int32 frame_subsampling_factor =
      utt_splitter->Config().frame_subsampling_factor;

  for (size_t c = 0; c < chunks.size(); c++) {
    const ChunkTimeInfo &chunk = chunks[c];

    int32 tot_input_frames = chunk.left_context + chunk.num_frames +
        chunk.right_context;

    Matrix<BaseFloat> input_frames(tot_input_frames, feats.NumCols(),
                                   kUndefined);

    int32 start_frame = chunk.first_frame - chunk.left_context;
    for (int32 t = start_frame; t < start_frame + tot_input_frames; t++) {
      int32 t2 = t;
      if (t2 < 0) t2 = 0;
      if (t2 >= num_input_frames) t2 = num_input_frames - 1;
      int32 j = t - start_frame;
      SubVector<BaseFloat> src(feats, t2),
          dest(input_frames, j);
      dest.CopyFromVec(src);
    }

    NnetExample eg;

    // call the regular input "input".
    eg.io.push_back(NnetIo("input", -chunk.left_context, input_frames));

    if (ivector_feats != NULL && ivector_as_input) {
      // if applicable, add the iVector feature.
      // choose iVector from a random frame in the chunk
      int32 ivector_frame = RandInt(start_frame,
                                    start_frame + num_input_frames - 1),
          ivector_frame_subsampled = ivector_frame / ivector_period;
      if (ivector_frame_subsampled < 0)
        ivector_frame_subsampled = 0;
      if (ivector_frame_subsampled >= ivector_feats->NumRows())
        ivector_frame_subsampled = ivector_feats->NumRows() - 1;
      Matrix<BaseFloat> ivector(1, ivector_feats->NumCols());
      ivector.Row(0).CopyFromVec(ivector_feats->Row(ivector_frame_subsampled));
      eg.io.push_back(NnetIo("ivector", 0, ivector));
    }

    // Note: chunk.first_frame and chunk.num_frames will both be
    // multiples of frame_subsampling_factor.
    int32 start_frame_subsampled = chunk.first_frame / frame_subsampling_factor,
        num_frames_subsampled = chunk.num_frames / frame_subsampling_factor;

    KALDI_ASSERT(start_frame_subsampled + num_frames_subsampled - 1 <
                 static_cast<int32>(pdf_post.size()));

    // Note: in all current cases there is no subsampling of output-frames going
    // on (--frame-subsampling-factor=1), so you could read
    // 'num_frames_subsampled' as just 'num_frames'.
    Posterior labels(num_frames_subsampled);

    // TODO: it may be that using these weights is not actually helpful (with
    // chain training, it was not), and that setting them all to 1 is better.
    // We could add a boolean option to this program to control that; but I
    // don't want to add such an option if experiments show that it is not
    // helpful.
    for (int32 i = 0; i < num_frames_subsampled; i++) {
      int32 t = i + start_frame_subsampled;
      labels[i] = pdf_post[t];
      for (std::vector<std::pair<int32, BaseFloat> >::iterator
               iter = labels[i].begin(); iter != labels[i].end(); ++iter)
        iter->second *= chunk.output_weights[i];
    }

    eg.io.push_back(NnetIo("output", num_pdfs, 0, labels));

    if (ivector_feats != NULL && ivector_as_output) {
      // if applicable, add the iVector feature.
      // choose iVector from a random frame in the chunk

      int32 ivector_frame = RandInt(start_frame,
                                    start_frame + num_input_frames - 1),
          ivector_frame_subsampled = ivector_frame / ivector_period;
      if (ivector_frame_subsampled < 0)
        ivector_frame_subsampled = 0;
      if (ivector_frame_subsampled >= ivector_feats->NumRows())
        ivector_frame_subsampled = ivector_feats->NumRows() - 1;
      Matrix<BaseFloat> ivector(num_frames_subsampled, ivector_feats->NumCols());
      ivector.CopyRowsFromVec(ivector_feats->Row(ivector_frame_subsampled));
      ivector.Scale(ivector_scale_factor);
      eg.io.push_back(NnetIo("ivector_aux_output", 0, ivector));
    }

    if (compress)
      eg.Compress();

    std::ostringstream os;
    os << utt_id << "-" << chunk.first_frame;

    std::string key = os.str(); // key is <utt_id>-<frame_id>

    example_writer->Write(key, eg);
  }
  return true;
}

} // namespace nnet3
} // namespace kaldi

int main(int argc, char *argv[]) {
  try {
    using namespace kaldi;
    using namespace kaldi::nnet3;
    typedef kaldi::int32 int32;
    typedef kaldi::int64 int64;
    typedef kaldi::float32 float32;

    const char *usage =
        "Get frame-by-frame examples of data for nnet3 neural network training.\n"
        "Essentially this is a format change from features and posteriors\n"
        "into a special frame-by-frame format.  This program handles the\n"
        "common case where you have some input features, possibly some\n"
        "iVectors, and one set of labels.  If people in future want to\n"
        "do different things they may have to extend this program or create\n"
        "different versions of it for different tasks (the egs format is quite\n"
        "general)\n"
        "\n"
        "Usage:  nnet3-get-egs [options] <features-rspecifier> "
        "<pdf-post-rspecifier> <egs-out>\n"
        "\n"
        "An example [where $feats expands to the actual features]:\n"
        "nnet3-get-egs --num-pdfs=2658 --left-context=12 --right-context=9 --num-frames=8 \"$feats\"\\\n"
        "\"ark:gunzip -c exp/nnet/ali.1.gz | ali-to-pdf exp/nnet/1.nnet ark:- ark:- | ali-to-post ark:- ark:- |\" \\\n"
        "   ark:- \n";


    bool compress = true, ivector_as_input = true, ivector_as_output = false;
    int32 num_pdfs = -1, length_tolerance = 100,
        online_ivector_period = 1;
    float32 ivector_scale_factor = 1.0;

    ExampleGenerationConfig eg_config;  // controls num-frames,
                                        // left/right-context, etc.

    std::string online_ivector_rspecifier;

    ParseOptions po(usage);

    po.Register("compress", &compress, "If true, write egs in "
                "compressed format (recommended).");
    po.Register("num-pdfs", &num_pdfs, "Number of pdfs in the acoustic "
                "model");
    po.Register("ivectors", &online_ivector_rspecifier, "Alias for "
                "--online-ivectors option, for back compatibility");
    po.Register("online-ivectors", &online_ivector_rspecifier, "Rspecifier of "
                "ivector features, as a matrix.");
    po.Register("online-ivector-period", &online_ivector_period, "Number of "
                "frames between iVectors in matrices supplied to the "
                "--online-ivectors option");
    po.Register("ivector-as-input", &ivector_as_input, "ivector added to the input of"
                "the neural netwrok");
    po.Register("ivector-as-output", &ivector_as_output, "ivector added to the output of"
                "the neural network as aux MSE training features");
    po.Register("ivector-scale-factor", &ivector_scale_factor, "factor used to scale the"
                "ivector value in the ivector_aux_output");
    po.Register("length-tolerance", &length_tolerance, "Tolerance for "
                "difference in num-frames between feat and ivector matrices");
    eg_config.Register(&po);

    po.Read(argc, argv);

    if (po.NumArgs() != 3) {
      po.PrintUsage();
      exit(1);
    }

    if (num_pdfs <= 0)
      KALDI_ERR << "--num-pdfs options is required.";

    eg_config.ComputeDerived();
    UtteranceSplitter utt_splitter(eg_config);

    std::string feature_rspecifier = po.GetArg(1),
        pdf_post_rspecifier = po.GetArg(2),
        examples_wspecifier = po.GetArg(3);

    // Read in all the training files.
    SequentialBaseFloatMatrixReader feat_reader(feature_rspecifier);
    RandomAccessPosteriorReader pdf_post_reader(pdf_post_rspecifier);
    NnetExampleWriter example_writer(examples_wspecifier);
    RandomAccessBaseFloatMatrixReader online_ivector_reader(
        online_ivector_rspecifier);

    int32 num_err = 0;

    for (; !feat_reader.Done(); feat_reader.Next()) {
      std::string key = feat_reader.Key();
      const Matrix<BaseFloat> &feats = feat_reader.Value();
      if (!pdf_post_reader.HasKey(key)) {
        KALDI_WARN << "No pdf-level posterior for key " << key;
        num_err++;
      } else {
        const Posterior &pdf_post = pdf_post_reader.Value(key);
        if (pdf_post.size() != feats.NumRows()) {
          KALDI_WARN << "Posterior has wrong size " << pdf_post.size()
                     << " versus " << feats.NumRows();
          num_err++;
          continue;
        }
        const Matrix<BaseFloat> *online_ivector_feats = NULL;
        if (!online_ivector_rspecifier.empty()) {
          if (!online_ivector_reader.HasKey(key)) {
            KALDI_WARN << "No iVectors for utterance " << key;
            num_err++;
            continue;
          } else {
            // this address will be valid until we call HasKey() or Value()
            // again.
            online_ivector_feats = &(online_ivector_reader.Value(key));
          }
        }

        if (online_ivector_feats != NULL &&
            (abs(feats.NumRows() - (online_ivector_feats->NumRows() *
                                    online_ivector_period)) > length_tolerance
             || online_ivector_feats->NumRows() == 0)) {
          KALDI_WARN << "Length difference between feats " << feats.NumRows()
                     << " and iVectors " << online_ivector_feats->NumRows()
                     << "exceeds tolerance " << length_tolerance;
          num_err++;
          continue;
        }

        if (!ProcessFile(feats, online_ivector_feats, online_ivector_period,
                         ivector_as_input, ivector_as_output, ivector_scale_factor,
                         pdf_post, key, compress, num_pdfs,
                         &utt_splitter, &example_writer))
            num_err++;
      }
    }
    if (num_err > 0)
      KALDI_WARN << num_err << " utterances had errors and could "
          "not be processed.";
    // utt_splitter prints stats in its destructor.
    return utt_splitter.ExitStatus();
  } catch(const std::exception &e) {
    std::cerr << e.what() << '\n';
    return -1;
  }
}
