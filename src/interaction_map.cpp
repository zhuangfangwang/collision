#pragma once

#include <limits>
#include <iostream>
#include <functional>
#include <algorithm>

#include <boost/range.hpp>
#include <boost/range/irange.hpp>
#include <boost/range/combine.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/numeric.hpp>

#include <boost/range/adaptor/indexed.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/adjacent_filtered.hpp>
//#include <boost/range/adaptors.hpp>       // somehow gives a link error?

#include "typedefs.cpp"
#include "numpy_eigen/array.cpp"
#include "numpy_boost/ndarray.cpp"
#include "numpy_boost/exception.cpp"
#include "maps.cpp"


using namespace boost;
using namespace boost::adaptors;


/*
vertex grid spatial collision map
this provides O(1) spatial lookups with no requirements at all on the point layout,
the datastructures are easy to construct and avoid dynamic allocation or cache-unfriendly structures

since we use this for surfaces embedded in 3d space, the virtual voxel grid is quite sparse
this makes a dense grid less attractive; plus the part of the datastructure that will require random access
fits snugly in L1 cache

the only way to make this faster would be to actively reorder the input points, to exploit temporal coherency in the lexsort
*/

template<typename real_t, typename fixed_t, int NDim>
class PointGrid {
	/*
	this datastructure allows for coarse/braod collision queries
	it is one of the most simple datastructures to implement and debug,
	and is chosen here with future GPU-porting in mind, as it maps well to parallel architectures
	*/

public:
	typedef int32_t                         index_t;       // 32 bit int is a fine size type; 4 billion points isnt very likely
	typedef int64_t                         hash_t;
	typedef Eigen::Array<real_t,  2, NDim>	box_t;
	typedef Eigen::Array<real_t,  1, NDim>	vector_t;
	typedef Eigen::Array<fixed_t, 1, NDim>	cell_t;
	typedef Eigen::Array<hash_t,  1, NDim>	strides_t;

	const ndarray<vector_t>      position;    // positions
	const index_t                n_points;    // number of points
	const real_t                 lengthscale; // size of a virtual voxel

	const box_t                  extents;     // maximum extent of pointcloud; used to map coordinates to positive integers
	const cell_t                 shape;       // number of virtual buckets in each direction; used to prevent out-of-bound lookup
	const strides_t              strides;     // for lex-ranking cells
public:
	const ndarray<cell_t>        cell_id;     // the cell coordinates a vertex resides in
	const ndarray<index_t>       permutation; // index array mapping the vertices to lexographically sorted order
	const ndarray<index_t>       pivots;	     // boundaries between buckets of cells as viewed under permutation
	const index_t                n_buckets;   // number of cells

	const HashMap<cell_t, index_t, NDim> bucket_from_cell; // maps cell coordinates to bucket index

public:
	//interface methods
	auto get_cells()        const { return cell_id.unview<fixed_t>(); }
	auto get_permutation()  const { return permutation; }
	auto get_pivots()       const { return pivots; }
	void set_cells      (ndarray<fixed_t, 2> cells)    {}
	void set_permutation(ndarray<index_t> permutation) {}
	void set_pivots     (ndarray<index_t> pivots)      {}

	// constructor
	explicit PointGrid(ndarray<real_t, 2> position, real_t lengthscale) :
		position	(position.view<vector_t>()),
		n_points	(position.size()),
		lengthscale	(lengthscale),
		extents		(init_extents()),
		shape		(init_shape()),
		strides		(init_strides()),
		cell_id		(init_cells()),
		permutation	(init_permutation()),
		pivots		(init_pivots()),
		n_buckets	(pivots.size() - 1),
		bucket_from_cell(       // create a map to invert the cell_from_bucket function
			boost::combine(
				irange(0, n_buckets) | transformed([&](auto b) {return cell_from_bucket(b);}),
				irange(0, n_buckets)
			)
		)
	{   // empty constructor; noice
	}

private:
	//determine extents of data
	auto init_extents() const {
		auto inf = std::numeric_limits<real_t>::infinity();
		box_t extents;
		extents.row(0).fill(+inf);
		extents.row(1).fill(-inf);
		for (auto p : position) {
			extents.row(0) = extents.row(0).min(p);
			extents.row(1) = extents.row(1).max(p);
		}
		return extents;
	}
	// integer shape of the domain
	cell_t init_shape() const {      // interestingly, using auto as return type fails spectacularly
		return transform(extents.row(1) - extents.row(0)).cast<fixed_t>() + 1;	// use +0.5 before cast?
	}
	// find strides for efficient lexsort
	auto init_strides() const {
		//		boost::partial_sum(shape.cast<int>(), begin(strides), std::multiplies<int>());   // doesnt work somehow
		strides_t strides;
		strides(0) = 1;
		for (auto i : irange(1, NDim))
			strides(i) = strides(i - 1) * shape(i - 1);
		return strides;
	}
	// determine grid cells
	auto init_cells() const {
		// silly indirection, because we cannot yet allocate custom type directly
		auto cell_id = ndarray<fixed_t, 2>({ n_points, NDim }).view<cell_t>();
		for (auto v : irange(0, n_points))
			cell_id[v] = cell_from_position(position[v]);
		return cell_id;
	}
	// finds the index vector that puts the vertices in a lexographically sorted order
	auto init_permutation() const {
		ndarray<index_t> permutation({ n_points });
		// init with initial order; 0 to n
		boost::copy(irange(0, n_points), permutation.begin());
		// branching-free lex sorting ftw
		auto _cell_id = cell_id.range();
		auto lex = [&](auto l, auto r) {return hash_from_cell(_cell_id[r] - _cell_id[l]) > 0;};
        // wow, casting permutation to raw range yield factor 3 performance in sorted case
		boost::sort(permutation.range(), lex);
		return permutation;
	}
	//divide the sorted vertices into buckets, containing vertices in the same virtual voxel
	auto init_pivots() const {
		// allocate array of size n_points, becuase it plays nicely with the rest of our numpy mempool
		ndarray<index_t> pivots({ n_points });

		index_t n_pivots = 0;		//number of pivots
		auto add_pivot = [&](auto p) {pivots[n_pivots++] = p;};

		auto res = permutation
			| transformed([&](auto i) {return cell_id[i];})
			| indexed(0)
			| adjacent_filtered([](auto a, auto b) {return (a.value() != b.value()).any();})
			| transformed([](auto i) {return i.index();});

		for (auto p : res)
			add_pivot(p);
		if (n_pivots == n_points)
			throw python_exception("every vertex is in its own cell; lengthscale probably needs to go way up");
		add_pivot(n_points);

		return pivots.resize(n_pivots);
	}


protected:
	//map a global coord into the grid local coords
	inline vector_t transform(const vector_t& v) const {
		return (v - extents.row(0)) / lengthscale;
	}
	inline cell_t cell_from_local_position(const vector_t& v) const {
		return v.cast<fixed_t>();	// we want to round towards zero; surprised that we do not need a -0.5 for that..
	}
	inline cell_t cell_from_position(const vector_t& v) const {
		return cell_from_local_position(transform(v));
	}
	//convert bucket index into cell coords
	inline cell_t cell_from_bucket(index_t b) const {
		return cell_id[permutation[pivots[b]]];
	}
	inline hash_t hash_from_cell(cell_t cell) const {
	    return (cell.cast<hash_t>() * strides).sum();
	}

protected:
	auto indices_from_bucket(index_t b) const {
		return (b == -1) ? irange(0, 0) : irange(pivots[b], pivots[b + 1]);
	}
	auto indices_from_cell(const cell_t& cell) const {
		return indices_from_bucket(bucket_from_cell[cell]);
	}
	auto vertices_from_cell(const cell_t& cell) const {
		return indices_from_cell(cell)
			| transformed([&](index_t i) {return permutation[i];});
	}

public:
	// public traversal interface; what this class is all about
	template <class F>
	void for_each_vertex_in_cell(const cell_t& cell, const F& body) const {
		for (auto v : vertices_from_cell(cell))
			body(v);
	}

	//loop over each occupied cell in the grid
	template <class F>
	void for_each_cell(const F& body) const {
		for (index_type b : irange(0, n_buckets))
			body(cell_from_bucket(b));
	}

	//loop over bounding box and apply body
	template <class F>
	void for_each_vertex_in_bounding_box(const box_t& box, const F& body) const
	{
	    auto gmin = box.row(0);
	    auto gmax = box.row(1);

		const auto in_box = [&](auto v)
		{
			const vector_t& vp = position[v];
			return !((vp < gmin).any() || (vp > gmax).any());
		};

		const vector_t lmin = transform(gmin);
		const vector_t lmax = transform(gmax);

		//intersected volume is not positive; bail
		if ((lmin.max(vector_t(0, 0, 0)) > lmax.min(shape.cast<real_t>())).any()) return;

		//compute local cell coords; constrain to [0-shape)
		const cell_t lb =  cell_from_local_position(lmin).max(cell_t(0, 0, 0));
		const cell_t ub = (cell_from_local_position(lmax) + 1).min(shape);

		//loop over all cells that intersect with bb
		for (auto x : irange(lb[0], ub[0]))
			for (auto y : irange(lb[1], ub[1]))
				for (auto z : irange(lb[2], ub[2]))
					for (auto v : vertices_from_cell(cell_t(x, y, z)))
						if (in_box(v))
							body(v);
	}
	//for unit testing purposes
	template <class F>
	void for_each_vertex_in_bounding_box_naive(const box_t& box, const F& body) const
	{
		const auto in_box = [&](auto v)
		{
			const vector_t& vp = position[v];
			return !((vp < box.row(0)).any() || (vp > box.row(1)).any());
		};

		if ((box.row(0) > extents.row(1)).any() || (box.row(1) < extents.row(0)).any()) return;

		for (auto v : irange(0, n_points))
			if (in_box(v))
				body(v);
	}
};
