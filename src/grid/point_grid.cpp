#pragma once

#include <functional>
#include <algorithm>

#include <boost/range.hpp>
#include <boost/range/irange.hpp>
#include <boost/range/algorithm.hpp>

#include <boost/range/adaptor/indexed.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/filtered.hpp>
//#include <boost/range/adaptors.hpp>       // somehow gives a link error?


#include "../typedefs.cpp"
#include "../numpy_eigen/array.cpp"
#include "../numpy_boost/ndarray.cpp"
#include "../numpy_boost/exception.cpp"

#include "grid_spec.cpp"
#include "sparse_grid.cpp"



using namespace boost;
using namespace boost::adaptors;


template<typename spec_t>
class PointGrid {
	/*
	provide spatial lookup in O(1) time for n-dimensional point clouds
	*/

public:
    typedef PointGrid<spec_t>				self_t;
    typedef typename spec_t::real_t         real_t;
	typedef typename spec_t::fixed_t		fixed_t;
	typedef typename spec_t::index_t		index_t;

	typedef typename spec_t::box_t			box_t;
	typedef typename spec_t::vector_t		vector_t;
	typedef typename spec_t::cell_t			cell_t;
	typedef erow<index_t, 2>                pair_t;

	const spec_t				 spec;
	const ndarray<vector_t>      position;    // positions
	const index_t                n_points;    // number of points

public:
	const ndarray<fixed_t>                  cell_id;     // the hash of a cell a point resides in
	const SparseGrid<fixed_t, index_t>      grid;        // defines buckets
	const ndarray<index_t>                  offsets;	 // determines stencil

public:
	//interface methods
	auto get_cells()        const { return cell_id; }
	auto get_permutation()  const { return grid.permutation; }


	// constructor
	explicit PointGrid(spec_t spec, ndarray<real_t, 2> position) :
		spec        (spec),
		position    (position.view<vector_t>()),
		n_points    (position.size()),
		cell_id     (init_cells()),
		grid        (cell_id)
	{
	}
	// constructor with nonzero stencil, for self intersection
	explicit PointGrid(spec_t spec, ndarray<real_t, 2> position, ndarray<index_t> offsets) :
		spec        (spec),
		position    (position.view<vector_t>()),
		n_points    (position.size()),
		cell_id     (init_cells()),
		offsets     (offsets),
		grid        (cell_id)
	{
	}

	// create a new pointgrid, using the permutation of existing pointgrid as initial guess
	self_t update(const ndarray<real_t, 2> position) const {
	    return self_t(spec, position, grid.permutation, offsets);
	}
	explicit PointGrid(spec_t spec, ndarray<real_t, 2> position, ndarray<index_t> permutation, ndarray<index_t> offsets) :
		spec		(spec),
		position	(position.view<vector_t>()),
		n_points	(position.size()),
		cell_id		(init_cells()),
		offsets     (offsets),
		grid        (cell_id, permutation)
	{
	}

private:
	// determine grid cells
	auto init_cells() const {
		auto cell_id = ndarray<fixed_t>({ n_points });
		for (index_t v : irange(0, n_points))
			cell_id[v] = spec.hash_from_cell(spec.cell_from_position(position[v]));
		return cell_id;
	}


public:
	// public traversal interface; what this class is all about
	template <class F>
	void for_each_point_in_cell(fixed_t cell, const F& body) const {
		for (index_t p : grid.indices_from_key(cell))
			body(p);
	}

	// symmetric iteration over all point pairs
	template <class F>
	void for_each_pair(const F& body) const {
		const real_t ls2 = spec.scale * spec.scale;

		const auto wrapper = [&](const index_t i, const index_t j){
			const vector_t rp = position[i] - position[j];
			const real_t d2 = (rp*rp).sum();
			if (d2 > ls2) return;
			body(i, j, d2);
		};

		//loop over all buckets
		for (const fixed_t ci : grid.unique_keys()) {
            const auto bi = grid.indices_from_key(ci);
			//interaction within bucket
			for (index_t pi : bi)
				for (index_t pj : bi)
					if (pi==pj) break; else
						wrapper(pi, pj);
			//loop over all neighboring buckets
			for (fixed_t o : offsets) {
				const auto bj = grid.indices_from_key(ci + o);
				for (const index_t pj : bj)		//loop over other guy first; he might be empty, giving early exit
					for (const index_t pi : bi)
						wrapper(pi, pj);
			}
		}
	}
    // compute [n, 2] array of all pairs within length_scale distance
	ndarray<index_t, 2> get_pairs() const {
	    std::vector<pair_t> pairs;
	    for_each_pair([&](index_t i, index_t j, real_t d2) {
	        pairs.push_back(pair_t(i, j));
	    });
	    index_t n_pairs(pairs.size());
        ndarray<index_t, 2> output({ n_pairs, 2});

        for (index_t p : irange(0, n_pairs)) {
            output[p][0] = pairs[p][0];
            output[p][1] = pairs[p][1];
        }
        return output;
	}

//	// self intersection
//	auto intersect() const {
//	}
//
//	auto intersect(const PointGrid& other) const {
//	}

//	auto intersect(const ObjectGrid& other) const {
//	}
};

