// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Starka
// Copyright (c) 2009-2014 Illumina, Inc.
//
// This software is provided under the terms and conditions of the
// Illumina Open Source Software License 1.
//
// You should have received a copy of the Illumina Open Source
// Software License 1 along with this program. If not, see
// <https://github.com/sequencing/licenses/>
//

/// \author Chris Saunders
///

#include "denovo_indel_caller.hh"
#include "blt_util/math_util.hh"
#include "blt_util/prob_util.hh"
#include "starling_common/indel_util.hh"
#include "starling_common/starling_diploid_indel.hh"
#include "starling_common/starling_indel_call_pprob_digt.hh"

#include <array>

//#define DENOVO_INDEL_DEBUG

#ifdef DENOVO_INDEL_DEBUG
#include "blt_util/log.hh"
#endif


#if 0

// For:
//
// homozygous state: S
// frequency grid: G
// number of allele axes: a = 1
// somatic grid size: |G|-a
//
// ln_som_match    = log( 1-P(S) )
// ln_som_mismatch = log( P(S)/(|G|-a) )
//
// :
// indel noise rate: n
// diploid set: D
// non-diploid noise points: N={G\D}
//
// lnp_norm[0..2]   = log( norm[0..2]*(1-n) )
// lnp_norm[3..|G|] = log( n/|N| )
//
//
somatic_indel_caller_grid::
somatic_indel_caller_grid(const strelka_options& opt,
                          const indel_digt_caller& in_caller)
{
    static const double error_mod( -std::log(static_cast<double>(STAR_DIINDEL_GRID::HET_RES*2)) );

    _ln_som_match=(log1p_switch(-opt.somatic_indel_rate));
    _ln_som_mismatch=(std::log(opt.somatic_indel_rate/(static_cast<double>(STAR_DIINDEL_GRID::SIZE-1))));

    std::fill(_bare_lnprior.normal.begin(),_bare_lnprior.normal.end(),0);
    std::fill(_bare_lnprior.normal_poly.begin(),_bare_lnprior.normal_poly.end(),0);

    const double* normal_lnprior_genomic(in_caller.lnprior_genomic());
    const double* normal_lnprior_polymorphic(in_caller.lnprior_polymorphic());

    for (unsigned ngt(0); ngt<STAR_DIINDEL::SIZE; ++ngt)
    {
        _bare_lnprior.normal[ngt] = normal_lnprior_genomic[ngt];
        _bare_lnprior.normal_poly[ngt] = normal_lnprior_polymorphic[ngt];
        //         _lnprior_normal_nonoise[ngt] = normal_lnprior[ngt];
    }

    for (unsigned ngt(STAR_DIINDEL::SIZE); ngt<STAR_DIINDEL_GRID::SIZE; ++ngt)
    {
        _bare_lnprior.normal[ngt] = error_mod;
        _bare_lnprior.normal_poly[ngt] = error_mod;
    }

#ifdef SOMATIC_DEBUG
    check_ln_distro(_lnprior_normal,
                    _lnprior_normal+STAR_DIINDEL_GRID::SIZE,
                    "somatic indel normal prior");
#endif
}

void
somatic_indel_caller_grid::
set_normal_prior(std::vector<blt_float_t>& normal_prior,
                 const double ref_error_prob,
                 const strelka_options& opt) const
{
    const double ln_sie_rate(opt.shared_indel_error_factor * std::log(ref_error_prob));
    const double ln_csie_rate(log1p_switch(-ln_sie_rate));

    for (unsigned ngt(0); ngt<STAR_DIINDEL::SIZE; ++ngt)
    {
        normal_prior[ngt] = _bare_lnprior.normal[ngt] + ln_csie_rate;
    }
    for (unsigned ngt(STAR_DIINDEL::SIZE); ngt<STAR_DIINDEL_GRID::SIZE; ++ngt)
    {
        normal_prior[ngt] = _bare_lnprior.normal[ngt] + ln_sie_rate;
    }
}


typedef somatic_indel_call::result_set result_set;



static
void
get_indel_het_grid_lhood(const starling_base_options& opt,
                         const starling_base_deriv_options& dopt,
                         const starling_sample_options& sample_opt,
                         const double indel_error_lnp,
                         const double indel_real_lnp,
                         const double ref_error_lnp,
                         const double ref_real_lnp,
                         const indel_key& ik,
                         const indel_data& id,
                         const bool is_include_tier2,
                         const bool is_use_alt_indel,
                         double* const lhood)
{
    static const unsigned lsize(STAR_DIINDEL_GRID::HET_RES*2);
    for (unsigned gt(0); gt<(lsize); ++gt) lhood[gt] = 0.;

    static const double ratio_increment(0.5/static_cast<double>(STAR_DIINDEL_GRID::HET_RES+1));
    for (unsigned i(0); i<STAR_DIINDEL_GRID::HET_RES; ++i)
    {
        const double het_ratio((i+1)*ratio_increment);
        indel_digt_caller::get_high_low_het_ratio_lhood(opt,dopt,
                                                        sample_opt,
                                                        indel_error_lnp,indel_real_lnp,
                                                        ref_error_lnp,ref_real_lnp,
                                                        ik,id,het_ratio,
                                                        is_include_tier2,is_use_alt_indel,
                                                        lhood[i],
                                                        lhood[lsize-(i+1)]);
    }
}
#endif




typedef std::array<double,STAR_DIINDEL::SIZE> indel_state_t;


namespace TRANSMISSION_STATE
{
    // "ERROR" represents a de-novo event that is incredibly unlikely (multiple events) -- we could also put it in the denovo state and just use the
    // de-novo prior squared to get the same result -- then the dominant term would actually be the probably of an erroneous copy
    // number observation in the sample instead.
    enum index_t
    {
        INHERITED,
        DENOVO,
        ERROR,
        SIZE
    };

    static
    const char*
    getLabel(
        const index_t idx)
    {
        switch (idx)
        {
        case INHERITED: return "INHERITED";
        case DENOVO: return "DENOVO";
        case ERROR: return "ERROR";
        default:
            assert(false && "Unknown transmission state");
            return nullptr;
        }
    }

    // temporary fixed priors:
    static
    double
    getPrior(
        const index_t idx)
    {
        // as currently defined background exp is I: 15/27 E: 2/27 D: 10/27 -- compared to drate this doesn't matter
        static const double lndrate(std::log(1e-8));
        switch (idx)
        {
        case INHERITED: return 0.;
        case DENOVO: return lndrate;
        case ERROR: return lndrate;
        default:
            assert(false && "Undefined inheritance state");
        }
    }

    static
    unsigned
    getEcount(
        const uint8_t* px,
        const uint8_t* py,
        const uint8_t* c)
    {
        unsigned val(0);
        if ((c[0] != px[0]) && (c[0] != px[1])) val += 1;
        if ((c[1] != py[0]) && (c[1] != py[1])) val += 1;
        return val;
    }

    static
    index_t
    get_state(
        const unsigned parent0GT,
        const unsigned parent1GT,
        const unsigned childGT)
    {
        static const unsigned alleleCount(2);
        uint8_t p0a[alleleCount];
        uint8_t p1a[alleleCount];
        uint8_t ca[alleleCount];
        for (unsigned alleleIndex(0); alleleIndex<alleleCount; ++alleleIndex)
        {
            p0a[alleleIndex] = STAR_DIINDEL::get_allele(parent0GT,alleleIndex);
            p1a[alleleIndex] = STAR_DIINDEL::get_allele(parent1GT,alleleIndex);
            ca[alleleIndex] = STAR_DIINDEL::get_allele(childGT,alleleIndex);
        }
        const unsigned ecount(std::min(getEcount(p0a,p1a,ca),getEcount(p1a,p0a,ca)));
        switch (ecount)
        {
        case 0: return INHERITED;
        case 1: return DENOVO;
        case 2: return ERROR;
        default:
            assert(false && "Unexpected count value");
            return ERROR;
        }
    }
}



#ifdef DENOVO_INDEL_DEBUG
static
void
dumpIndelLhood(
    const char* label,
    const double* lhood,
    std::ostream& os)
{
    using namespace STAR_DIINDEL;
    os << label << " indel lhood: ref/het/hom: " << lhood[NOINDEL] << " " << lhood[HET] << " " << lhood[HOM] << "\n";
}
#endif



static
void
calculate_result_set(
    const SampleInfoManager& sinfo,
    const std::vector<indel_state_t>& sampleLhood,
    denovo_indel_call::result_set& rs)
{
    using namespace INOVO_SAMPLETYPE;

    const unsigned probandIndex(sinfo.getTypeIndexList(PROBAND)[0]);
    const std::vector<unsigned>& parentIndex(sinfo.getTypeIndexList(PARENT));

    static const double lnzero(-std::numeric_limits<double>::infinity());
    std::array<double,TRANSMISSION_STATE::SIZE> stateLhood;
    std::fill(stateLhood.begin(),stateLhood.end(),lnzero);

#ifdef DENOVO_INDEL_DEBUG
    dumpIndelLhood("parent0", sampleLhood[parentIndex[0]].data(), log_os);
    dumpIndelLhood("parent1", sampleLhood[parentIndex[1]].data(), log_os);
    dumpIndelLhood("proband", sampleLhood[probandIndex].data(), log_os);
#endif

    // just go for total brute force as a first pass at this:
    for (unsigned p0(0); p0<STAR_DIINDEL::SIZE; ++p0)
    {
        for (unsigned p1(0); p1<STAR_DIINDEL::SIZE; ++p1)
        {
            for (unsigned pro(0); pro<STAR_DIINDEL::SIZE; ++pro)
            {
                const double pedigreeLhood = sampleLhood[parentIndex[0]][p0] + sampleLhood[parentIndex[1]][p1] + sampleLhood[probandIndex][pro];
                const TRANSMISSION_STATE::index_t tran(TRANSMISSION_STATE::get_state(p0,p1,pro));
#ifdef DENOVO_INDEL_DEBUG
                {
                    using namespace TRANSMISSION_STATE;
                    log_os << "p0/p1/c: "
                            << STAR_DIINDEL::get_gt_label(p0) << " "
                            << STAR_DIINDEL::get_gt_label(p1) << " "
                            << STAR_DIINDEL::get_gt_label(pro) << " trans_state: " << getLabel(tran) << " lhood: " << pedigreeLhood << "\n";
                }
#endif
                stateLhood[tran] = log_sum(stateLhood[tran],pedigreeLhood);
            }
        }
    }

    std::array<double,TRANSMISSION_STATE::SIZE> statePprob;
    for (unsigned tstate(0); tstate<TRANSMISSION_STATE::SIZE; ++tstate)
    {
        statePprob[tstate] = stateLhood[tstate] + TRANSMISSION_STATE::getPrior(static_cast<TRANSMISSION_STATE::index_t>(tstate));
#ifdef DENOVO_INDEL_DEBUG
        const TRANSMISSION_STATE::index_t tidx(static_cast<TRANSMISSION_STATE::index_t>(tstate));
        log_os << "denovo state pprob/lhood/prior: " << TRANSMISSION_STATE::getLabel(tidx)
               << " " << statePprob[tstate] << " " << stateLhood[tstate]
               << " " << TRANSMISSION_STATE::getPrior(tidx) << "\n";
#endif
    }

    //opt_normalize_ln_distro(pprob.begin(),pprob.end(),DDIINDEL_GRID::is_nonsom.val.begin(),rs.max_gt);
    normalize_ln_distro(statePprob.begin(),statePprob.end(),rs.max_gt);

#ifdef DEBUG_INDEL_CALL
    log_os << "INDEL_CALL pprob(noindel),pprob(hom),pprob(het): " << pprob[STAR_DIINDEL::NOINDEL] << " " << pprob[STAR_DIINDEL::HOM] << " " << pprob[STAR_DIINDEL::HET] << "\n";
#endif
    rs.dindel_qphred=error_prob_to_qphred(statePprob[TRANSMISSION_STATE::INHERITED] + statePprob[TRANSMISSION_STATE::ERROR]);
}


#ifdef SOMATIC_DEBUG
static
void
debug_dump_indel_lhood(
    const double* lhood,
    std::ostream& os)
{
    double pprob[STAR_DIINDEL_GRID::SIZE];
    for (unsigned gt(0); gt<STAR_DIINDEL_GRID::SIZE; ++gt)
    {
        pprob[gt] = lhood[gt];
    }

    unsigned max_gt(0);
    normalize_ln_distro(pprob,pprob+STAR_DIINDEL_GRID::SIZE,max_gt);

    os << std::setprecision(3) << std::fixed;
    for (unsigned gt(0); gt<STAR_DIINDEL_GRID::SIZE; ++gt)
    {
        os << static_cast<STAR_DIINDEL::index_t>(STAR_DIINDEL_GRID::get_star_diindel_state(gt)) << ": " << -std::log(pprob[gt]) << " ";
        os << "\n";
    }
    os.unsetf(std::ios::fixed);
}
#endif




static
bool
is_multi_indel_allele(
    const starling_base_deriv_options& dopt,
    const std::vector<const indel_data*>& allIndelData,
    const bool is_include_tier2,
    bool& is_overlap)
{
    static const bool is_use_alt_indel(true);
    static const double min_explained_count_fraction(.9);

    enum iallele_t
    {
        INDEL = -2,
        REF = -1
    };

    // get total pprob:
    read_path_scores total_pprob;
    const unsigned sampleSize(allIndelData.size());
    for (unsigned sampleIndex(0); sampleIndex<sampleSize; ++sampleIndex)
    {
        const bool isInit(sampleIndex==0);
        get_sum_path_pprob(dopt, *(allIndelData[sampleIndex]), is_include_tier2, is_use_alt_indel, total_pprob, isInit);
    }

    // next determine the top two indel alleles:
    std::vector<std::pair<double,int> > scores;
    scores.push_back(std::make_pair(-total_pprob.indel,static_cast<int>(INDEL)));
    scores.push_back(std::make_pair(-total_pprob.ref,static_cast<int>(REF)));
    const read_path_scores::alt_indel_t& ai(total_pprob.alt_indel);
    const int ais(ai.size());
    for (int i(0); i<ais; ++i)
    {
        scores.push_back(std::make_pair(-ai[i].second,i));
    }

    sort(scores.begin(),scores.end());

#if 0
    std::cerr << "BUG: normal_id: " << normal_id;
    std::cerr << "BUG: tumor_id: " << tumor_id;
    std::cerr << "BUG: total_pprob: " << total_pprob << "\n";
    std::cerr << "BUG: max1_id,max2_id " << scores[0].second << " " << scores[1].second << "\n";
#endif

    // If the top two alleles are both alternate indels, check that
    // they interfere with each other.  If not, we are forced to make
    // the conservative assumption that they occur as part of the same
    // haplotype:
    //
    assert(scores.size() >= 2);
    while (scores[0].second>=0 && scores[1].second>=0)
    {
        if (is_indel_conflict(ai[scores[0].second].first,ai[scores[1].second].first))
        {
            break;
        }
        scores.erase(scores.begin()+1);
        assert(scores.size() >= 2);
    }

    if ((scores[0].second!=INDEL) && (scores[1].second!=INDEL)) return true;
    if (scores.size() >= 3)
    {
        const double top_prob(scores[0].first+scores[1].first);
        const double top_frac(top_prob/(top_prob+scores[2].first));
        if (top_frac<min_explained_count_fraction) return true;
    }


    // the rejection criteria is resolved at this point, but one more
    // flag is set below as an interesting utility for users to
    // quickly find/filter the 'legitimate' overlapping indels:
    //
    is_overlap=((scores[0].second!=REF) && (scores[1].second!=REF));

    return false;
}



///
///
void
get_denovo_indel_call(
    const inovo_options& opt,
    const inovo_deriv_options& dopt,
    const SampleInfoManager& sinfo,
    const std::vector<const starling_sample_options*>& sampleOptions,
    const double indel_error_prob,
    const double ref_error_prob,
    const indel_key& ik,
    const std::vector<const indel_data*>& allIndelData,
    const bool is_use_alt_indel,
    denovo_indel_call& dinc)
{
    // for now, lhood calculation of each sample is independent:

    // get likelihood of each genotype
    static const bool is_het_bias(false);
    static const double het_bias(0.0);

    // set is_forced_output
    for (const indel_data* idp : allIndelData)
    {
        if (! idp->is_forced_output) continue;
        dinc.is_forced_output=true;
        break;
    }

    // put some tier structure in for future, but only use 1 at present:
    static const unsigned n_tier(1);
    std::array<denovo_indel_call::result_set,n_tier> tier_rs;
    for (unsigned tierIndex(0); tierIndex<n_tier; ++tierIndex)
    {
        const bool is_include_tier2(tierIndex==1);

        // early escape filter borrowed directly from somatic case
        static const bool is_denovo_multi_indel_filter(true);
#if 0
        std::cerr << "BUG: testing tier/ik: " << i << " " << ik;
#endif
        if (is_denovo_multi_indel_filter)
        {
            const bool ismulti(is_multi_indel_allele(dopt,allIndelData,is_include_tier2,dinc.rs.is_overlap));
            if (ismulti)
            {
                tier_rs[tierIndex].dindel_qphred=0;
#if 0
                std::cerr << "BUG: rejected\n";
#endif
                continue;
            }
        }

        const unsigned sampleSize(allIndelData.size());
        std::vector<indel_state_t> sampleLhood(sampleSize);

        for (unsigned sampleIndex(0);sampleIndex<sampleSize;++sampleIndex)
        {
            indel_digt_caller::get_indel_digt_lhood(
                opt,dopt,*(sampleOptions[sampleIndex]),
                indel_error_prob,ref_error_prob,ik,
                *(allIndelData[sampleIndex]),
                is_het_bias, het_bias,
                is_include_tier2, is_use_alt_indel,
                sampleLhood[sampleIndex].data());
        }

        calculate_result_set(sinfo, sampleLhood, tier_rs[tierIndex]);
    }

    if (! dinc.is_forced_output)
    {
        for (const auto& val : tier_rs)
        {
            if (val.dindel_qphred==0) return;
        }
    }

    dinc.rs=tier_rs[0];

#if 0
    const double indel_error_lnp(std::log(indel_error_prob));
    const double indel_real_lnp(std::log(1.-indel_error_prob));
    const double ref_error_lnp(std::log(ref_error_prob));
    const double ref_real_lnp(std::log(1.-ref_error_prob));

    static const unsigned n_tier(2);
    std::array<result_set,n_tier> tier_rs;
    for (unsigned i(0); i<n_tier; ++i)
    {
        const bool is_include_tier2(i==1);
        if (is_include_tier2)
        {
            if (! opt.is_tier2()) continue;
            if (tier_rs[0].sindel_qphred==0)
            {
                if (! dinc.is_forced_output)   // if forced output then there's still a point to computing tier2
                {
                    tier_rs[1].sindel_qphred=0;
                    continue;
                }
            }
        }

        static const bool is_somatic_multi_indel_filter(true);
#if 0
        std::cerr << "BUG: testing tier/ik: " << i << " " << ik;
#endif
        if (is_somatic_multi_indel_filter)
        {
            const bool ismulti(is_multi_indel_allele(dopt,normal_id,tumor_id,is_include_tier2,tier_rs[i].is_overlap));
            if (ismulti)
            {
                tier_rs[i].sindel_qphred=0;
#if 0
                std::cerr << "BUG: rejected\n";
#endif
                continue;
            }
        }

        indel_digt_caller::get_indel_digt_lhood(opt,dopt,normal_opt,
                                                indel_error_prob,ref_error_prob,ik,normal_id,
                                                is_normal_het_bias,normal_het_bias,
                                                is_include_tier2,is_use_alt_indel,
                                                normal_lhood);
        indel_digt_caller::get_indel_digt_lhood(opt,dopt,tumor_opt,
                                                indel_error_prob,ref_error_prob,ik,tumor_id,
                                                is_tumor_het_bias,tumor_het_bias,
                                                is_include_tier2,is_use_alt_indel,
                                                tumor_lhood);

        get_indel_het_grid_lhood(opt,dopt,normal_opt,
                                 indel_error_lnp,indel_real_lnp,
                                 ref_error_lnp,ref_real_lnp,
                                 ik,normal_id,
                                 is_include_tier2,is_use_alt_indel,
                                 normal_lhood+STAR_DIINDEL::SIZE);
        get_indel_het_grid_lhood(opt,dopt,tumor_opt,
                                 indel_error_lnp,indel_real_lnp,
                                 ref_error_lnp,ref_real_lnp,
                                 ik,tumor_id,
                                 is_include_tier2,is_use_alt_indel,
                                 tumor_lhood+STAR_DIINDEL::SIZE);

        set_normal_prior(normal_prior,ref_error_prob, opt);

        calculate_result_set(normal_prior,
                             _ln_som_match,_ln_som_mismatch,
                             normal_lhood,tumor_lhood,tier_rs[i]);
    }

    if (! sindel.is_forced_output)
    {
        if (tier_rs[0].sindel_qphred==0 ||
            tier_rs[1].sindel_qphred==0) return;
    }

    sindel.sindel_tier=0;
    if (opt.is_tier2())
    {
        if (tier_rs[0].sindel_qphred > tier_rs[1].sindel_qphred)
        {
            sindel.sindel_tier=1;
        }
    }

    sindel.sindel_from_ntype_tier=0;
    if (opt.is_tier2())
    {
        if (tier_rs[0].sindel_from_ntype_qphred > tier_rs[1].sindel_from_ntype_qphred)
        {
            sindel.sindel_from_ntype_tier=1;
        }
    }

    sindel.rs=tier_rs[sindel.sindel_from_ntype_tier];

    if (tier_rs[0].ntype != tier_rs[1].ntype)
    {
        // catch NTYPE conflict states:
        sindel.rs.ntype = NTYPE::CONFLICT;
        sindel.rs.sindel_from_ntype_qphred = 0;

    }
    else
    {
        // classify NTYPE:
        //

        // convert diploid genotype into more limited ntype set:
        //
        if       (sindel.rs.ntype==STAR_DIINDEL::NOINDEL)
        {
            sindel.rs.ntype=NTYPE::REF;
        }
        else if (sindel.rs.ntype==STAR_DIINDEL::HOM)
        {
            sindel.rs.ntype=NTYPE::HOM;
        }
        else
        {
            sindel.rs.ntype=NTYPE::HET;
#if 0
        }
        else if (sindel.rs.ntype==STAR_DIINDEL::HET)
        {
            sindel.rs.ntype=NTYPE::HET;
        }
        else
        {
            assert(0);
#endif
        }
    }

    sindel.rs.sindel_qphred = tier_rs[sindel.sindel_tier].sindel_qphred;
#endif
}