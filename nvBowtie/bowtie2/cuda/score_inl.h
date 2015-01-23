/*
 * nvbio
 * Copyright (c) 2011-2014, NVIDIA CORPORATION. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the NVIDIA CORPORATION nor the
 *      names of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <nvBowtie/bowtie2/cuda/alignment_utils.h>
#include <nvBowtie/bowtie2/cuda/pipeline_states.h>

namespace nvbio {
namespace bowtie2 {
namespace cuda {

namespace detail {

///@addtogroup nvBowtie
///@{

///@addtogroup Scoring
///@{

///@addtogroup ScoringDetail
///@{

///
/// A scoring stream, fetching the input hits to score from the hit queue
/// indexed by the input sorting order, and assigning them their score and sink
/// attributes.
///
template <typename AlignerType, typename PipelineType>
struct BestScoreStream : public AlignmentStreamBase<SCORE_STREAM,AlignerType,PipelineType>
{
    typedef AlignmentStreamBase<SCORE_STREAM,AlignerType,PipelineType>  base_type;
    typedef typename base_type::context_type                            context_type;
    typedef typename base_type::scheme_type                             scheme_type;

    /// constructor
    ///
    /// \param _band_len            effective band length;
    ///                             NOTE: this value must match the template BAND_LEN parameter
    ///                             used for instantiating aln::BatchedBandedAlignmentScore.
    ///
    /// \param _pipeline            the pipeline object
    ///
    /// \param _aligner             the aligner object
    ///
    BestScoreStream(
        const uint32                _band_len,
        const PipelineType          _pipeline,
        const AlignerType           _aligner,
        const ParamsPOD             _params) :
        base_type( _pipeline, _aligner, _params ), m_band_len( _band_len ) {}

    /// return the maximum pattern length
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 max_pattern_length() const { return base_type::m_pipeline.reads.max_read_len(); }

    /// return the maximum text length
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 max_text_length() const { return base_type::m_pipeline.reads.max_read_len() + m_band_len; }

    /// return the stream size
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 size() const { return base_type::m_pipeline.hits_queue_size; }

    /// initialize the i-th context
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    bool init_context(
        const uint32    i,
        context_type*   context) const
    {
        context->idx = base_type::m_pipeline.idx_queue[i];

        // fetch the hit to process
        HitReference<HitQueuesDeviceView> hit = base_type::m_pipeline.scoring_queues.hits[ context->idx ];

        // setup the read info
        context->mate         = 0u;
        context->read_rc      = hit.seed.rc;
        context->read_id      = hit.read_id;
        context->read_range   = base_type::m_pipeline.reads.get_range( context->read_id );

        // setup the genome range
        const uint32 g_pos = hit.loc;

        const uint32 read_len = context->read_range.y - context->read_range.x;
        context->genome_begin = g_pos > m_band_len/2 ? g_pos - m_band_len/2 : 0u;
        context->genome_end   = nvbio::min( context->genome_begin + m_band_len + read_len, base_type::m_pipeline.genome_length );

        // initialize the sink
        context->sink = aln::BestSink<int32>();

        // setup the minimum score
        const io::Alignment second_best = base_type::m_pipeline.best_alignments[ context->read_id + base_type::m_pipeline.best_stride ];
        context->min_score = nvbio::max( second_best.score(), base_type::m_pipeline.score_limit );

        NVBIO_CUDA_DEBUG_PRINT_IF( base_type::m_params.debug.show_score_info( context->read_id ), "score-min: %d (rc[%u], pos[%u], [qid %u])]\n", context->min_score, context->read_rc, context->genome_begin, i );
        return true;
    }

    /// handle the output
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    void output(
        const uint32        i,
        const context_type* context) const
    {
        // write the final hit.score and hit.sink attributes
        HitReference<HitQueuesDeviceView> hit = base_type::m_pipeline.scoring_queues.hits[ context->idx ];

        const aln::BestSink<int32> sink = context->sink;
        hit.score = nvbio::max( sink.score, scheme_type::worst_score );
        hit.sink  = context->genome_begin + sink.sink.x;

        // TODO: rewrite hit.loc
        // hit.loc = context->genome_begin;
        NVBIO_CUDA_DEBUG_PRINT_IF( base_type::m_params.debug.show_score( context->read_id, (sink.score >= context->min_score) ), "score: %d (rc[%u], pos[%u], [qid %u])]\n", sink.score, context->read_rc, context->genome_begin, i );
    }

    const uint32 m_band_len;
};

///
/// A scoring stream, fetching the input hits to score from the hit queue
/// indexed by the input sorting order, and assigning them their score and sink
/// attributes.
///
template <typename AlignerType, typename PipelineType>
struct BestAnchorScoreStream : public AlignmentStreamBase<SCORE_STREAM,AlignerType,PipelineType>
{
    typedef AlignmentStreamBase<SCORE_STREAM,AlignerType,PipelineType>  base_type;
    typedef typename base_type::context_type                            context_type;
    typedef typename base_type::scheme_type                             scheme_type;

    /// constructor
    ///
    /// \param _band_len            effective band length;
    ///                             NOTE: this value must match the template BAND_LEN parameter
    ///                             used for instantiating aln::BatchedBandedAlignmentScore.
    ///
    /// \param _pipeline            the pipeline object
    ///
    /// \param _aligner             the aligner object
    ///
    BestAnchorScoreStream(
        const uint32                _band_len,
        const PipelineType          _pipeline,
        const AlignerType           _aligner,
        const ParamsPOD             _params) :
        base_type( _pipeline, _aligner, _params ), m_band_len( _band_len ) {}

    /// return the maximum pattern length
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 max_pattern_length() const { return base_type::m_pipeline.reads.max_read_len(); }

    /// return the maximum text length
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 max_text_length() const { return base_type::m_pipeline.reads.max_read_len() + m_band_len; }

    /// return the stream size
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 size() const { return base_type::m_pipeline.hits_queue_size; }

    /// initialize the i-th context
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    bool init_context(
        const uint32    i,
        context_type*   context) const
    {
        context->idx = base_type::m_pipeline.idx_queue[i];

        // initialize the sink
        context->sink = aln::BestSink<int32>();

        // fetch the hit to process
        HitReference<HitQueuesDeviceView> hit = base_type::m_pipeline.scoring_queues.hits[ context->idx ];

        const uint32 read_rc = hit.seed.rc;
        const uint32 read_id = hit.read_id;

        const io::BestPairedAlignments best = io::BestPairedAlignments(
            io::BestAlignments( base_type::m_pipeline.best_alignments[ read_id ], base_type::m_pipeline.best_alignments[ read_id + base_type::m_pipeline.best_stride ] ),
            io::BestAlignments( base_type::m_pipeline.best_alignments_o[ read_id ], base_type::m_pipeline.best_alignments_o[ read_id + base_type::m_pipeline.best_stride ] ) );

        // compute the optimal score of the opposite mate
        const uint2  o_read_range    = base_type::m_pipeline.reads_o.get_range( read_id );
        const uint32 o_read_len      = o_read_range.y - o_read_range.x;
        const  int32 o_optimal_score = base_type::m_pipeline.scoring_scheme.perfect_score( o_read_len );
        const  int32 o_worst_score   = base_type::m_pipeline.scoring_scheme.min_score( o_read_len );

        const uint2  a_read_range    = base_type::m_pipeline.reads.get_range( read_id );
        const uint32 a_read_len      = a_read_range.y - a_read_range.x;
        const  int32 a_optimal_score = base_type::m_pipeline.scoring_scheme.perfect_score( a_read_len );
        const  int32 a_worst_score   = base_type::m_pipeline.scoring_scheme.min_score( a_read_len );

        const  int32 target_pair_score = nvbio::min(
            compute_target_score( best, a_worst_score, o_worst_score ) + 1,
            a_optimal_score + o_optimal_score );

        int32 target_mate_score = target_pair_score - o_optimal_score;

      #if 1
        //
        // bound the score of this mate by the worst score allowed for its own read length.
        // NOTE: disabling this is equivalent to allowing a worst score proportional to the total
        // length of the read pair.
        //

        target_mate_score = nvbio::max( target_mate_score, a_worst_score );
      #endif

        // setup the read info
        context->mate       = 0;
        context->read_rc    = read_rc;
        context->read_id    = read_id;
        context->read_range = a_read_range;

        // setup the genome range
        const uint32 g_pos = hit.loc;

        context->genome_begin = g_pos > m_band_len/2 ? g_pos - m_band_len/2 : 0u;
        context->genome_end   = nvbio::min( context->genome_begin + m_band_len + a_read_len, base_type::m_pipeline.genome_length );

        // skip locations that we have already visited
        const uint32 mate = base_type::m_pipeline.anchor;
        const bool skip = (mate == best.m_a1.m_mate && (read_rc == best.m_a1.m_rc && g_pos == best.m_a1.m_align)) ||
                          (mate == best.m_o1.m_mate && (read_rc == best.m_o1.m_rc && g_pos == best.m_o1.m_align)) ||
                          (mate == best.m_a2.m_mate && (read_rc == best.m_a2.m_rc && g_pos == best.m_a2.m_align)) ||
                          (mate == best.m_o2.m_mate && (read_rc == best.m_o2.m_rc && g_pos == best.m_o2.m_align)) ||
                          (context->min_score > a_optimal_score);

        // setup the minimum score
        context->min_score = skip ? Field_traits<int32>::max() : nvbio::max( target_mate_score, base_type::m_pipeline.score_limit );

        return !skip;
    }

    /// handle the output
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    void output(
        const uint32        i,
        const context_type* context) const
    {
        // write the final hit.score and hit.sink attributes
        HitReference<HitQueuesDeviceView> hit = base_type::m_pipeline.scoring_queues.hits[ context->idx ];

        const aln::BestSink<int32> sink = context->sink;
        hit.score = (sink.score >= context->min_score) ? sink.score : scheme_type::worst_score;
        hit.sink  = context->genome_begin + sink.sink.x;
        // TODO: rewrite hit.loc
        // hit.loc = context->genome_begin;
        NVBIO_CUDA_DEBUG_PRINT_IF( base_type::m_params.debug.show_score( context->read_id, (sink.score >= context->min_score) ), "score anchor: %d (min[%d], mate[%u], rc[%u], pos[%u], [qid %u])\n", sink.score, context->min_score, base_type::m_pipeline.anchor, context->read_rc, context->genome_begin, i );
    }

    const uint32 m_band_len;
};


///
/// A scoring stream, fetching the input hits to score from the hit queue
/// indexed by the input sorting order, and assigning them their score and sink
/// attributes.
///
template <typename AlignerType, typename PipelineType>
struct BestOppositeScoreStream : public AlignmentStreamBase<OPPOSITE_SCORE_STREAM,AlignerType,PipelineType>
{
    typedef AlignmentStreamBase<OPPOSITE_SCORE_STREAM,AlignerType,PipelineType>  base_type;
    typedef typename base_type::context_type                            context_type;
    typedef typename base_type::scheme_type                             scheme_type;

    /// constructor
    ///
    /// \param _band_len            effective band length;
    ///                             NOTE: this value must match the template BAND_LEN parameter
    ///                             used for instantiating aln::BatchedBandedAlignmentScore.
    ///
    /// \param _pipeline            the pipeline object
    ///
    /// \param _aligner             the aligner object
    ///
    BestOppositeScoreStream(
        const PipelineType          _pipeline,
        const AlignerType           _aligner,
        const ParamsPOD             _params) :
        base_type( _pipeline, _aligner, _params ) {}

    /// return the maximum pattern length
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 max_pattern_length() const { return base_type::m_pipeline.reads_o.max_sequence_len(); }

    /// return the maximum text length
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 max_text_length() const { return base_type::m_params.max_frag_len; }

    /// return the stream size
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 size() const { return base_type::m_pipeline.opposite_queue_size; }

    /// initialize the i-th context
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    bool init_context(
        const uint32    i,
        context_type*   context) const
    {
        context->idx = base_type::m_pipeline.idx_queue[ base_type::m_pipeline.opposite_queue[i] ];

        // initialize score and sink
        context->sink.invalidate();

        // fetch the hit to process
        HitReference<HitQueuesDeviceView> hit = base_type::m_pipeline.scoring_queues.hits[ context->idx ];

        const uint32 read_rc = hit.seed.rc;
        const uint32 read_id = hit.read_id;

        const uint32 g_pos = hit.loc;

        const uint2  a_read_range    = base_type::m_pipeline.reads.get_range( read_id );
        const uint32 a_len           = a_read_range.y - a_read_range.x;
        const  int32 a_optimal_score = base_type::m_pipeline.scoring_scheme.perfect_score( a_len );
        const  int32 a_worst_score   = base_type::m_pipeline.scoring_scheme.min_score( a_len );

        const uint2  o_read_range    = base_type::m_pipeline.reads_o.get_range( read_id );
        const uint32 o_len           = o_read_range.y - o_read_range.x;
        const  int32 o_optimal_score = base_type::m_pipeline.scoring_scheme.perfect_score( o_len );
        const  int32 o_worst_score   = base_type::m_pipeline.scoring_scheme.min_score( o_len );

        // compute the pair score threshold
        const int32 anchor_score = hit.score;

        const io::BestPairedAlignments best = io::BestPairedAlignments(
            io::BestAlignments( base_type::m_pipeline.best_alignments[ read_id ], base_type::m_pipeline.best_alignments[ read_id + base_type::m_pipeline.best_stride ] ),
            io::BestAlignments( base_type::m_pipeline.best_alignments_o[ read_id ], base_type::m_pipeline.best_alignments_o[ read_id + base_type::m_pipeline.best_stride ] ) );

        const int32 target_pair_score = nvbio::min(
            compute_target_score( best, a_worst_score, o_worst_score ) + 1,
            a_optimal_score + o_optimal_score );

        int32 target_mate_score = target_pair_score - anchor_score;

      #if 1
        //
        // bound the score of this mate by the worst score allowed for its own read length.
        // NOTE: disabling this is equivalent to allowing a worst score proportional to the total
        // length of the read pair.
        //

        target_mate_score = nvbio::max( target_mate_score, o_worst_score );
      #endif

        // assign the final score threshold
        context->min_score = nvbio::max( target_mate_score, base_type::m_pipeline.score_limit );

      #if DP_REPORT_MULTIPLE
        context->sink.set_min_score( context->min_score );
      #endif

        // check if it's even possible to reach the score threshold
        if (context->min_score > o_optimal_score)
        {
            NVBIO_CUDA_DEBUG_PRINT_IF( base_type::m_params.debug.show_score( context->read_id, false ), "score opposite: min-score too high: %d > %d (mate[%u], rc[%u], [qid %u])\n", context->min_score, o_optimal_score, base_type::m_pipeline.anchor ? 0u : 1u, context->read_rc, i );
            return false;
        }

        // frame the alignment
        bool o_left;
        bool o_fw;

        frame_opposite_mate(
            base_type::m_params.pe_policy,
            base_type::m_pipeline.anchor,
            !read_rc,
            o_left,
            o_fw );

        // setup the read info
        context->mate       = 1u;
        context->read_rc    = !o_fw;
        context->read_id    = read_id;
        context->read_range = o_read_range;

        // FIXME: re-implement the logic of Bowtie2's otherMate() function!
        const int32 max_ref_gaps = aln::max_text_gaps( base_type::aligner(), context->min_score, o_len );
        const uint32 o_gapped_len = o_len + max_ref_gaps;

        if (o_left)
        {
            const uint32 max_end = g_pos + a_len + o_gapped_len > base_type::m_params.min_frag_len ? g_pos + a_len + o_gapped_len - base_type::m_params.min_frag_len : 0u;
            context->genome_begin = g_pos + a_len > base_type::m_params.max_frag_len ? (g_pos + a_len) - base_type::m_params.max_frag_len : 0u;
            context->genome_end   = base_type::m_params.pe_overlap ? g_pos + a_len : g_pos;
            context->genome_end   = nvbio::min( context->genome_end, max_end );
        }
        else
        {
            const uint32 min_begin = g_pos + base_type::m_params.min_frag_len > o_gapped_len ? g_pos + base_type::m_params.min_frag_len - o_gapped_len : 0u;
            context->genome_end   = g_pos + base_type::m_params.max_frag_len;
            context->genome_begin = base_type::m_params.pe_overlap ? g_pos : g_pos + a_len;
            context->genome_begin = nvbio::max( context->genome_begin, min_begin );
        }
        context->genome_end = nvbio::min( context->genome_end, base_type::m_pipeline.genome_length );

        // don't align if completely outside genome
        if (context->genome_begin >= base_type::m_pipeline.genome_length)
            return false;

        // bound against genome end
        context->genome_end = nvbio::min( context->genome_end, base_type::m_pipeline.genome_length );

        // skip locations that we have already visited
        const uint32 mate = base_type::m_pipeline.anchor ? 0u : 1u;
        const bool skip = (mate == best.m_a1.m_mate && (context->read_rc == best.m_a1.m_rc && g_pos == best.m_a1.m_align)) ||
                          (mate == best.m_o1.m_mate && (context->read_rc == best.m_o1.m_rc && g_pos == best.m_o1.m_align)) ||
                          (mate == best.m_a2.m_mate && (context->read_rc == best.m_a2.m_rc && g_pos == best.m_a2.m_align)) ||
                          (mate == best.m_o2.m_mate && (context->read_rc == best.m_o2.m_rc && g_pos == best.m_o2.m_align)) ||
                          (context->genome_begin == context->genome_end);

        return !skip;
    }

    /// handle the output
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    void output(
        const uint32        i,
        const context_type* context) const
    {
        // write the final hit.score and hit.sink attributes
        HitReference<HitQueuesDeviceView> hit = base_type::m_pipeline.scoring_queues.hits[ context->idx ];

    #if DP_REPORT_MULTIPLE
        const aln::BestColumnSink<int32,20>& sink = context->sink;
        uint32 best_idx1, best_idx2;

        sink.best2( best_idx1, best_idx2, (context->read_range.y - context->read_range.x) / 2u );

        const uint32 genome_sink  = sink.sinks[best_idx1].x != uint32(-1) ? sink.sinks[best_idx1].x : 0u;
        const uint32 genome_sink2 = sink.sinks[best_idx2].x != uint32(-1) ? sink.sinks[best_idx2].x : 0u;

        hit.opposite_score  = (sink.scores[best_idx1] >= context->min_score) ? sink.scores[best_idx1] : scheme_type::worst_score;
        hit.opposite_score2 = (sink.scores[best_idx2] >= context->min_score) ? sink.scores[best_idx2] : scheme_type::worst_score;
    #else
        const aln::BestSink<int32> sink = context->sink;
        const uint32 genome_sink  = sink.sink.x != uint32(-1) ? sink.sink.x : 0u;
        const uint32 genome_sink2 = 0u;

        hit.opposite_score  = (sink.score >= context->min_score) ? sink.score : scheme_type::worst_score;
        hit.opposite_score2 = scheme_type::worst_score;
    #endif

        hit.opposite_loc   = context->genome_begin;
        hit.opposite_sink  = context->genome_begin + genome_sink;
        hit.opposite_sink2 = context->genome_begin + genome_sink2;
        NVBIO_CUDA_DEBUG_PRINT_IF( base_type::m_params.debug.show_score( context->read_id, (sink.score >= context->min_score) ), "score opposite: %d (min[%d], mate[%u], rc[%u], pos[%u:%u], [qid %u])\n", sink.score, context->min_score, base_type::m_pipeline.anchor ? 0u : 1u, context->read_rc, context->genome_begin, context->genome_end, i );
    }
};

///
/// dispatch the execution of a batch of single-ended banded-alignment score calculations
///
template <typename aligner_type, typename pipeline_type>
void banded_score_best(
    const uint32         band_len,
    const pipeline_type& pipeline,
    const aligner_type   aligner,
    const ParamsPOD      params)
{
    const uint32 static_band_len = 
        (band_len < 4)  ? 3u  :
        (band_len < 8)  ? 7u  :
        (band_len < 16) ? 15u :
                          31u;

    typedef BestScoreStream<aligner_type,pipeline_type> stream_type;

    stream_type stream(
        static_band_len,
        pipeline,
        aligner,
        params );

    typedef aln::DeviceThreadScheduler scheduler_type;
    //typedef aln::DeviceStagedThreadScheduler scheduler_type;

    if (band_len < 4)
    {
        aln::BatchedBandedAlignmentScore<3u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else if (band_len < 8)
    {
        aln::BatchedBandedAlignmentScore<7u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else if (band_len < 16)
    {
        aln::BatchedBandedAlignmentScore<15u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else
    {
        aln::BatchedBandedAlignmentScore<31u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
}

///
/// dispatch the execution of a batch of banded-alignment score calculations for the anchor mates
///
template <typename aligner_type, typename pipeline_type>
void banded_anchor_score_best(
    const uint32         band_len,
    const pipeline_type& pipeline,
    const aligner_type   aligner,
    const ParamsPOD      params)
{
    const uint32 static_band_len = 
        (band_len < 4)  ? 3u  :
        (band_len < 8)  ? 7u  :
        (band_len < 16) ? 15u :
                          31u;

    typedef BestAnchorScoreStream<aligner_type,pipeline_type> stream_type;

    stream_type stream(
        static_band_len,
        pipeline,
        aligner,
        params );

    typedef aln::DeviceThreadScheduler scheduler_type;
    //typedef aln::DeviceStagedThreadScheduler scheduler_type;

    if (band_len < 4)
    {
        aln::BatchedBandedAlignmentScore<3u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else if (band_len < 8)
    {
        aln::BatchedBandedAlignmentScore<7u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else if (band_len < 16)
    {
        aln::BatchedBandedAlignmentScore<15u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else
    {
        aln::BatchedBandedAlignmentScore<31u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
}

///
/// dispatch the execution of a batch of alignment score calculations for the opposite mates
///
template <typename aligner_type, typename pipeline_type>
void opposite_score_best(
    const pipeline_type& pipeline,
    const aligner_type   aligner,
    const ParamsPOD      params)
{
    typedef BestOppositeScoreStream<aligner_type,pipeline_type> stream_type;

    stream_type stream(
        pipeline,
        aligner,
        params );

    aln::BatchedAlignmentScore<stream_type, aln::DeviceThreadBlockScheduler<128,9> > batch;
    //aln::BatchedAlignmentScore<stream_type, aln::DeviceStagedThreadScheduler> batch;

    batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
}

///
/// A scoring stream, fetching the input hits to score from the hit queue
/// indexed by the input sorting order, and assigning them their score and sink
/// attributes.
///
template <typename AlignerType, typename PipelineType>
struct AllScoreStream : public AlignmentStreamBase<SCORE_STREAM,AlignerType,PipelineType>
{
    typedef AlignmentStreamBase<SCORE_STREAM,AlignerType,PipelineType>  base_type;
    typedef typename base_type::context_type                            context_type;
    typedef typename base_type::scheme_type                             scheme_type;

    /// constructor
    ///
    /// \param _band_len            effective band length;
    ///                             NOTE: this value must match the template BAND_LEN parameter
    ///                             used for instantiating aln::BatchedBandedAlignmentScore.
    ///
    /// \param _pipeline            the pipeline object
    ///
    /// \param _aligner             the aligner object
    ///
    AllScoreStream(
        const uint32                _band_len,
        const PipelineType          _pipeline,
        const AlignerType           _aligner,
        const ParamsPOD             _params,
        const uint32                _buffer_offset,
        const uint32                _buffer_size,
              uint32*               _counter) :
        base_type( _pipeline, _aligner, _params ), m_band_len( _band_len ),
        m_buffer_offset( _buffer_offset ),
        m_buffer_size( _buffer_size ),
        m_counter( _counter ) {}

    /// return the maximum pattern length
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 max_pattern_length() const { return base_type::m_pipeline.reads.max_sequence_len(); }

    /// return the maximum text length
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 max_text_length() const { return base_type::m_pipeline.reads.max_sequence_len() + m_band_len; }

    /// return the stream size
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 size() const { return base_type::m_pipeline.hits_queue_size; }

    /// initialize the i-th context
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    bool init_context(
        const uint32    i,
        context_type*   context) const
    {
        context->idx = base_type::m_pipeline.idx_queue[i];

        // initialize the sink
        context->sink = aln::BestSink<int32>();

        // fetch the hit to process
        HitReference<HitQueuesDeviceView> hit = base_type::m_pipeline.scoring_queues.hits[ context->idx ];

        // setup the read info
        context->mate         = 0;
        context->read_rc      = hit.seed.rc;
        context->read_id      = hit.read_id;
        context->read_range   = base_type::m_pipeline.reads.get_range( context->read_id );

        // setup the genome range
        const uint32 g_pos = hit.loc;

        const uint32 read_len = context->read_range.y - context->read_range.x;
        context->genome_begin = g_pos > m_band_len/2 ? g_pos - m_band_len/2 : 0u;
        context->genome_end   = nvbio::min( context->genome_begin + m_band_len + read_len, base_type::m_pipeline.genome_length );

        // setup the minimum score
        context->min_score = base_type::m_pipeline.scoring_scheme.min_score( read_len );
        return true;
    }

    /// handle the output
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    void output(
        const uint32        i,
        const context_type* context) const
    {
        const aln::BestSink<int32> sink = context->sink;

        // append all valid alignments to a list
        if (sink.score >= context->min_score)
        {
            HitReference<HitQueuesDeviceView> hit = base_type::m_pipeline.scoring_queues.hits[ context->idx ];

          #if defined(NVBIO_DEVICE_COMPILATION)
            const uint32 slot = atomicAdd( m_counter, 1u );
          #else
            const uint32 slot = (*m_counter)++;
          #endif
            base_type::m_pipeline.buffer_read_info[ (m_buffer_offset + slot) % m_buffer_size ]  = hit.read_id; //(hit.read_id << 1) | hit.seed.rc;
            base_type::m_pipeline.buffer_alignments[ (m_buffer_offset + slot) % m_buffer_size ] = io::Alignment( hit.loc, 0u, sink.score, hit.seed.rc );
            // TODO: rewrite hit.loc
            // hit.loc = context->genome_begin;
            NVBIO_CUDA_DEBUG_PRINT_IF( base_type::m_params.debug.show_score( context->read_id, (sink.score >= context->min_score) ), "score: %d (rc[%u], pos[%u], [qid %u])]\n", sink.score, context->read_rc, context->genome_begin, i );
        }
    }

    const uint32  m_band_len;
    const uint32  m_buffer_offset;
    const uint32  m_buffer_size;
          uint32* m_counter;
};

///
/// dispatch the execution of a batch of single-ended banded-alignment score calculations
///
template <typename aligner_type, typename pipeline_type>
void banded_score_all(
    const uint32         band_len,
    const pipeline_type& pipeline,
    const aligner_type   aligner,
    const ParamsPOD      params,
    const uint32         buffer_offset,
    const uint32         buffer_size,
          uint32*        counter)
{
    const uint32 static_band_len = 
        (band_len < 4)  ? 3u  :
        (band_len < 8)  ? 7u  :
        (band_len < 16) ? 15u :
                          31u;

    typedef AllScoreStream<aligner_type,pipeline_type> stream_type;

    stream_type stream(
        static_band_len,
        pipeline,
        aligner,
        params,
        buffer_offset,
        buffer_size,
        counter );

    //typedef aln::DeviceThreadScheduler scheduler_type;
    typedef aln::DeviceStagedThreadScheduler scheduler_type;

    if (band_len < 4)
    {
        aln::BatchedBandedAlignmentScore<3u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else if (band_len < 8)
    {
        aln::BatchedBandedAlignmentScore<7u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else if (band_len < 16)
    {
        aln::BatchedBandedAlignmentScore<15u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
    else
    {
        aln::BatchedBandedAlignmentScore<31u,stream_type,scheduler_type> batch;

        batch.enact( stream, pipeline.dp_buffer_size, pipeline.dp_buffer );
    }
}

///@}  // group ScoringDetail
///@}  // group Scoring
///@}  // group nvBowtie

} // namespace detail

//
// execute a batch of single-ended banded-alignment score calculations, best mapping
//
template <typename scheme_type>
void score_best_t(
    const uint32                                        band_len,
    const BestApproxScoringPipelineState<scheme_type>&  pipeline,
    const ParamsPOD                                     params)
{
    if (params.alignment_type == LocalAlignment)
    {
        detail::banded_score_best(
            band_len,
            pipeline,
            pipeline.scoring_scheme.local_aligner(),
            params );
    }
    else
    {
        detail::banded_score_best(
            band_len,
            pipeline,
            pipeline.scoring_scheme.end_to_end_aligner(),
            params );
    }
}

//
// execute a batch of banded-alignment score calculations for the anchor mates, best mapping
//
template <typename scheme_type>
void anchor_score_best_t(
    const uint32                                        band_len,
    const BestApproxScoringPipelineState<scheme_type>&  pipeline,
    const ParamsPOD                                     params)
{
    if (params.alignment_type == LocalAlignment)
    {
        detail::banded_anchor_score_best(
            band_len,
            pipeline,
            pipeline.scoring_scheme.local_aligner(),
            params );
    }
    else
    {
        detail::banded_anchor_score_best(
            band_len,
            pipeline,
            pipeline.scoring_scheme.end_to_end_aligner(),
            params );
    }
}

//
// execute a batch of single-ended banded-alignment score calculations, best mapping
//
// \param band_len             alignment band length
// \param pipeline             all mapping pipeline
// \param params               alignment params
// \param buffer_offset        ring buffer offset
// \param buffer_size          ring buffer size
// \return                     number of valid alignments
template <typename scheme_type>
uint32 score_all_t(
    const uint32                                        band_len,
    const AllMappingPipelineState<scheme_type>&         pipeline,
    const ParamsPOD                                     params,
    const uint32                                        ring_offset,
    const uint32                                        ring_size)
{
    thrust::device_vector<uint32> counter(1,0);

    if (params.alignment_type == LocalAlignment)
    {
        detail::banded_score_all(
            band_len,
            pipeline,
            pipeline.scoring_scheme.local_aligner(),
            params,
            ring_offset,
            ring_size,
            nvbio::device_view(counter) );
    }
    else
    {
        detail::banded_score_all(
            band_len,
            pipeline,
            pipeline.scoring_scheme.end_to_end_aligner(),
            params,
            ring_offset,
            ring_size,
            nvbio::device_view(counter) );
    }
    //cudaDeviceSynchronize();
    //nvbio::cuda::check_error("score kernel");
    return counter[0];
}

} // namespace cuda
} // namespace bowtie2
} // namespace nvbio
