/*
 Implicit skinning
 Copyright (C) 2013 Rodolphe Vaillant, Loic Barthe, Florian Cannezin,
 Gael Guennebaud, Marie Paule Cani, Damien Rohmer, Brian Wyvill,
 Olivier Gourmel

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License 3 as published by
 the Free Software Foundation.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>
 */
/// @file animesh_potential.hpp
/// @warning should be only included once in the project
/// @brief holds the skinning algorithm to project mesh vertices onto
/// the implicit surface

#include "skeleton_env_evaluator.hpp"
#include "animesh_enum.hpp"
#include "cuda_utils.hpp"
#include "ray_cu.hpp"
#include "bone.hpp"
#include "conversions.hpp"

#include <math_constants.h>

// Max number of steps for the vertices fit with the dichotomie
#define DICHOTOMIE (20)

#define EPSILON 0.0001f

#define ENABLE_COLOR

#define ENABLE_MARCH

/** @file animation_potential.hpp
 *  Various functions that are used to compute the potential at some point of
 *  the animated skeleton. This file MUST be included in the main cuda
    program file due to the use of cuda textures. Use the header
    animation_kernels.hpp in order to call the kernels outside the cuda main
    programm file (cuda_main_kernel.cu)
 */

// =============================================================================
namespace Animesh_kers {
// =============================================================================

typedef Skeleton_env::Std_bone_eval Std_eval;

/// Evaluate skeleton potential
__device__
float eval_potential(const Point_cu& p, Vec3_cu& grad)
{
    return Skeleton_env::compute_potential<Std_eval>(p, grad);
}

// -----------------------------------------------------------------------------

/// Computes the potential at each vertex of the mesh. When the mesh is
/// animated, if implicit skinning is enabled, vertices move so as to match that
/// value of the potential.
__global__
void compute_base_potential(const Point_cu* in_verts,
                            const int nb_verts,
                            float* base_potential,
                            Vec3_cu* base_grad)
{
    const int p = blockIdx.x * blockDim.x + threadIdx.x;
    if(p < nb_verts)
    {
        float f = eval_potential(in_verts[p], base_grad[p]);
        base_potential[p] = f;
    }
}

// -----------------------------------------------------------------------------

__device__
float dichotomic_search(const Ray_cu&r,
                        float t0, float t1,
                        Vec3_cu& grad,
                        float iso)
{
    float t = t0;
    float f0 = eval_potential(r(t0), grad);
    float f1 = eval_potential(r(t1), grad);

    if(f0 > f1){
        t0 = t1;
        t1 = t;
    }

    Point_cu p;
    for(unsigned short i = 0 ; i < DICHOTOMIE; ++i)
    {
        t = (t0 + t1) * 0.5f;
        p = r(t);
        f0 = eval_potential(p, grad);

        if(f0 > iso){
            t1 = t;
            if((f0-iso) < EPSILON) break;
        } else {
            t0 = t;
            if((iso-f0) < EPSILON) break;
        }
    }
    return t;
}

// -----------------------------------------------------------------------------

/// Search for the gradient divergence section
__device__
float dichotomic_search_div(const Ray_cu&r,
                            float t0, float t1,
                            Vec3_cu& grad1,
                            float threshold)
{
    //#define FROM_START
    float t;
    Vec3_cu grad0, grad;
    float f = eval_potential(r(t0), grad0);

    Point_cu p;
    for(unsigned short i = 0; i < DICHOTOMIE; ++i)
    {
        t = (t0 + t1) * 0.5f;
        p = r(t);
        f = eval_potential(p, grad);

        if(grad.dot(grad0) > threshold)
        {
            t1 = t;
            grad1 = grad;
        }
        else if(grad.dot(grad1) > threshold)
        {
            t0 = t;
            grad0 = grad;
        }
        else
            break;// No more divergence maybe its a false collision ?
    }
    #ifdef FROM_START
    grad = grad0;
    return t0; // if
    #else
    return t;
    #endif
}

// -----------------------------------------------------------------------------

/// transform iso to sfactor
__device__
inline static float iso_to_sfactor(float x, int s)
{
     #if 0
    x = fabsf(x);
    // We compute : 1-(x^c0 - 1)^c1
    // with c0=2 and c1=4 (note: c0 and c1 are 'slopes' at point x=0 and x=1 )
    x *= x; // x^2
    x = (x-1.f); // (x^2 - 1)
    x *= x; // (x^2 - 1)^2
    x *= x; // (x^2 - 1)^4
    return (x > 1.f) ? 1.f : 1.f - x/* (x^2 - 1)^4 */;
    #elif 1
    x = fabsf(x);
    // We compute : 1-(x^c0 - 1)^c1
    // with c0=1 and c1=4 (note: c0 and c1 are 'slopes' at point x=0 and x=1 )
    //x *= x; // x^2
    x = (x-1.f); // (x^2 - 1)
    float res = 1.f;
    for(int i = 0; i < s; i++) res *= x;
    x = res; // (x^2 - 1)^s
    return (x > 1.f) ? 1.f : 1.f - x/* (x^2 - 1)^s */;
    #else
    return 1.f;
    #endif
}

// -----------------------------------------------------------------------------

#if 1
/*
    Ajustement standard avec gradient
*/

/// Move the vertices along a mix between their normals and the joint rotation
/// direction in order to match their base potential at rest position
/// @param d_output_vertices  vertices array to be moved in place.
/// @param d_ssd_interpolation_factor  interpolation weights for each vertices
/// which defines interpolation between ssd animation and implicit skinning
/// 1 is full ssd and 0 full implicit skinning
/// @param do_tune_direction if false use the normal to displace vertices
/// @param gradient_threshold when the mesh's points are fitted they march along
/// the gradient of the implicit primitive. this parameter specify when the vertex
/// stops the march i.e when gradient_threshold < to the scalar product of the
/// gradient between two steps
/// @param full_eval tells is we evaluate the skeleton entirely or if we just
/// use the potential of the two nearest clusters, in full eval we don't update
/// d_vert_to_fit has it is suppossed to be the last pass
__global__
void match_base_potential(const bool full_fit,
                          const bool smooth_fac_from_iso,
                          Vec3_cu* out_verts,
                          const float* base_potential,
                          const Vec3_cu* custom_dir,
                          Vec3_cu* out_gradient,
                          const Skeleton_env::DBone_id* nearest_bone,
                          float* smooth_factors_iso,
                          float* smooth_factors,
                          int* vert_to_fit,
                          const int nb_vert_to_fit,
                          const unsigned short nb_iter,
                          const float gradient_threshold,
                          const float step_length,
                          const bool potential_pit, // TODO: this condition should not be necessary
                          int* d_vert_state,
                          const float smooth_strength,
                          const float collision_depth,
                          const int slope,
                          const bool raphson)
{
#ifdef ENABLE_MARCH
    const int thread_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if(thread_idx < nb_vert_to_fit)
    {
        const int p = vert_to_fit[thread_idx];

        // STOP CASE : Vertex already fitted
        if(p == -1) return;

//        const Skeleton_env::DBone_id nearest = full_eval ? Skeleton_env::DBone_id(-1) : nearest_bone[p];
        const float ptl = base_potential[p];

        Point_cu v0 = out_verts[p].to_point();
        Vec3_cu gf0;
        float f0;
        f0 = eval_potential(v0, gf0) - ptl;

        if(smooth_fac_from_iso)
            smooth_factors_iso[p] = iso_to_sfactor(f0, slope) * smooth_strength;

        out_gradient[p] = gf0;
        // STOP CASE : Gradient is null we can't know where to march
        if(gf0.norm() <= 0.00001f){
            if(!full_fit) vert_to_fit[thread_idx] = -1;
            #ifdef ENABLE_COLOR
            d_vert_state[p] = EAnimesh::NORM_GRAD_NULL;
            #endif
            return;
        }

        // STOP CASE : Point already near enough the isosurface
        if( fabsf(f0) < EPSILON ){
            if(!full_fit) vert_to_fit[thread_idx] = -1;
            #ifdef ENABLE_COLOR
            d_vert_state[p] = EAnimesh::NOT_DISPLACED;
            #endif
            return;
        }

        #ifdef ENABLE_COLOR
        d_vert_state[p] = EAnimesh::NB_ITER_MAX;
        #endif

        // Inside we march along the inverted gradient
        // outside along the gradient :
        const float dl = (f0 > 0.f) ? -step_length : step_length;

        Ray_cu r;
        r.set_pos(v0);
        float t = 0.f;

        Vec3_cu  gfi    = gf0;
        float    fi     = f0;
        float    abs_f0 = fabsf(f0);
        Point_cu vi     = v0;

        //const Vec3_cu c_dir = custom_dir[p];

        for(unsigned short i = 0; i < nb_iter; ++i)
        {

            r.set_pos(v0);
            if( raphson ){
                float nm = gf0.norm_squared();
                r.set_dir( gf0 );
                t = dl * abs_f0 / nm;
                //t = t < 0.001f ? dl : t;
            } else {
                #if 1
                    r.set_dir( gf0.normalized() );
                #else
                    if( gf0.dot( c_dir ) > 0.f ) r.set_dir(  c_dir );
                    else                         r.set_dir( -c_dir );

                #endif
                t = dl;
            }

            vi = r(t);
            fi = eval_potential(vi, gfi) - ptl;

            // STOP CASE 1 : Initial iso-surface reached
            abs_f0 = fabsf(fi);
            if(raphson && abs_f0 < EPSILON )
            {
                if(!full_fit) vert_to_fit[thread_idx] = -1;
                #ifdef ENABLE_COLOR
                d_vert_state[p] = EAnimesh::OUT_VERT;
                #endif
                break;
            }
            else if( fi * f0 <= 0.f)
            {
                t = dichotomic_search(r, 0.f, t, gfi, ptl);

                if(!full_fit) vert_to_fit[thread_idx] = -1;

                #ifdef ENABLE_COLOR
                d_vert_state[p] = EAnimesh::FITTED;
                #endif
                break;
            }

            // STOP CASE 2 : Gradient divergence
            if( (gf0.normalized()).dot(gfi.normalized()) < gradient_threshold)
            {
                #if 0
                t = dichotomic_search_div(r, -step_length, t, .0,
                                          gtmp, gradient_threshold);
                #endif

                if(!full_fit) vert_to_fit[thread_idx] = -1;

                smooth_factors[p] = smooth_strength;
                #ifdef ENABLE_COLOR
                d_vert_state[p] = EAnimesh::GRADIENT_DIVERGENCE;
                #endif
                break;
            }

            // STOP CASE 3 : Potential pit
            if( ((fi - f0)*dl < 0.f) & potential_pit )
            {
                if(!full_fit) vert_to_fit[thread_idx] = -1;
                smooth_factors[p] = smooth_strength;
                #ifdef ENABLE_COLOR
                d_vert_state[p] = EAnimesh::POTENTIAL_PIT;
                #endif
                break;
            }

            v0  = vi;
            f0  = fi;
            gf0 = gfi;

            if(gf0.norm_squared() < (0.001f*0.001f)) break;
        }

        const Point_cu res = r(t);
        out_gradient[p] = gfi;
        out_verts[p] = res;
    }
#endif
}
#endif

// -----------------------------------------------------------------------------

/// @brief Raytracing interface to evaluate the whole skeleton
struct Skeleton_potential{
    __device__
    static float f(const Point_cu& p){
        Vec3_cu grad;
        return Skeleton_env::compute_potential<Std_eval>(p, grad);
    }

    __device__
    static Vec3_cu gf(const Point_cu& p){
        Vec3_cu grad;
        Skeleton_env::compute_potential<Std_eval>(p, grad);
        return grad;
    }

    __device__
    static float fngf(const Point_cu& p, Vec3_cu& gf){
        return Skeleton_env::compute_potential<Std_eval>(p, gf);
    }
};

}
// END KERNELS NAMESPACE =======================================================

// BEGIN CUDA FACTS : WHAT FOLLOWS CANNOT BE MOVED IN A NAMESPACE ==============

/// Ids of the Bones to be raytrace in device memory
#define NB_BONES_MAX 8
// Well sometimes with cuda constant mem you need to put __device__ before
// the declaration sometimes you don't... You'll just have to see if you end-up
// with corrupted memory inside your kernels...
// I know sometimes cuda feels like lol omg wtf XD
__device__ __constant__ int bone_ids___[NB_BONES_MAX];
/*__device__ */__constant__ int nb_bones___[1];

/** @class Partial_bone_eval
  @brief static class for evaluating bones in the list bone_ids___[]
  @see Skeleton_partial_eval::set_bones_to_raytrace()
*/
class Partial_bone_eval{
public:
    __device__
    static float f(Skeleton_env::DBone_id id_bone, Vec3_cu& grad, const Point_cu& pt){
        for(int i = 0; i < nb_bones___[0]; i++){
            if(Skeleton_env::DBone_id( bone_ids___[i] ) == id_bone){
                return Skeleton_env::fetch_and_eval_bone(id_bone, grad, pt);
            }
        }
        grad = Vec3_cu(0.f, 0.f, 0.f);
        return 0.f;
    }
};


/** @class Skeleton_partial_eval
  @brief static class used to raytrace a limited list of bones
  This class is used by the raytracer to draw a list of bones defined with
  the function set_bones_to_raytrace()

  @see Skeleton_partial_eval::set_bones_to_raytrace()
*/
struct Skeleton_partial_eval{

    __device__
    static float f(const Point_cu& p){
        Vec3_cu gf;
        return Skeleton_env::compute_potential<Partial_bone_eval>(p, gf);
    }

    __device__
    static Vec3_cu gf(const Point_cu& p){
        Vec3_cu gf;
        Skeleton_env::compute_potential<Partial_bone_eval>(p, gf);
        return gf;
    }

    __device__
    static float fngf(const Point_cu& p, Vec3_cu& gf){
        return Skeleton_env::compute_potential<Partial_bone_eval>(p, gf);
    }

    /// set bones to raytrace by taking at most the eight last entries of
    /// std::vector 'bone_ids'.
    __host__
    static void set_bones_to_raytrace(const std::vector<int>& set)
    {
        std::vector<int> ids_set = set;

        assert( ids_set.size() > 0 );
        int nb_bones = min((int)ids_set.size(), NB_BONES_MAX);
        int array[NB_BONES_MAX];
        int acc = 0;
        for(int i = (ids_set.size()-1); (i >= 0) && (acc < NB_BONES_MAX); i--)
        {
            Skeleton_env::DBone_id device_id = Skeleton_env::bone_hidx_to_didx( 0, ids_set[i] );//////////////////////////////////////////////// TODO: use real skel instance id
            //std::cout << device_id << std::endl;//DEBUG
            if( device_id.is_valid() )
            {
                array[acc] = device_id.id();
                acc++;
            }
        }

        // Copy to device constant memory
        CUDA_SAFE_CALL( cudaMemcpyToSymbol(bone_ids___, array,     sizeof(int)*NB_BONES_MAX) );
        CUDA_SAFE_CALL( cudaMemcpyToSymbol(nb_bones___, &nb_bones, sizeof(int)) );
    }

};

// END CUDA FACTS ==============================================================