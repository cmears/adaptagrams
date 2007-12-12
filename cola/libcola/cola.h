#ifndef COLA_H
#define COLA_H

#include <utility>
#include <iterator>
#include <vector>
#include <valarray>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <cassert>
#include "gradient_projection.h"
#include "cluster.h"
#include "straightener.h"

namespace vpsc { class Rectangle; }
namespace topology { 
    class Node; 
    class Edge;
    class TopologyConstraints;
}

/**
 * The cola namespace delineates the interface to the libcola constraint layout library
 */
namespace cola {
using vpsc::Rectangle;
using std::vector;
using std::valarray;

//! Edges are simply a pair of indices to entries in the Node vector
typedef std::pair<unsigned, unsigned> Edge;

/**
 * provides a functor that is called before each iteration in the main loop of
 * the ConstrainedMajorizationLayout::run() method.
 * Keeps a local copy of the x and y GradientProjection instances.
 * Override the operator() for things like locking the position of nodes
 * just for the duration of the iteration.
 * If the operator() returns false the subsequent iterations are
 * abandoned... ie layout ends immediately.  You can make it return true
 * e.g. when a user-interrupt is detected.
 */ 
struct Lock {
    unsigned id;
    double x,y;
    Lock(unsigned id, double x, double y) : id(id), x(x), y(y) {}
};
typedef vector<Lock> Locks;
/** 
 * provides a functor for callback before each iteration
 */
class PreIteration {
public:
    PreIteration(Locks& locks) : locks(locks) {}
    virtual ~PreIteration() {}
    virtual bool operator()() {
        return true;
    }
    Locks& locks;
private:
};
/** 
 * provides a functor for callback after each iteration
 * You can either call ConstrainedMajorizationLayout with an instance of this class setting the
 * tolerance and maxiterations as desired, or create a derived class implementing the operator() to
 * do your own convergence test, or create your own operator() that calls the
 * TestConvergence::operator() in order to do any other post processing you might need... e.g. to
 * animate changes.
 */
class TestConvergence {
public:
    double old_stress;
    TestConvergence(const double tolerance = 1e-4, const unsigned maxiterations = 100)
        : tolerance(tolerance),
          maxiterations(maxiterations)
    { reset(); }
    virtual ~TestConvergence() {}

    virtual bool operator()(const double new_stress, valarray<double> & X, valarray<double> & Y) {
        //std::cout<<"iteration="<<iterations<<", old_stress="<<old_stress<<", new_stress="<<new_stress<<std::endl;
        if (old_stress == DBL_MAX) {
            old_stress = new_stress;
            return ++iterations >= maxiterations;
        }
        // converged if relative decrease in stress falls below threshold
        // or if stress increases (shouldn't happen for straight majorization)
        bool converged = 
            (old_stress - new_stress) / (new_stress + 1e-10) < tolerance
             || 
            ++iterations > maxiterations;
        old_stress = new_stress;
        return converged;
    }
    void reset() {
        old_stress = DBL_MAX;
        iterations = 0;
    }
    const double tolerance;
    const unsigned maxiterations;
    unsigned iterations;
};

//! default instance of TestConvergence used if no other is specified
static TestConvergence defaultTest(0.0001,100);

/**
 * The following class implements the Constrained Majorization graph Layout algorithm.
 * The optimisation method used is "stress majorization", where a sequence of quadratic
 * functions which strictly bound the stress from above, is solved to monotonically reduce
 * the stress (by iteratively changing the configuration of nodes).
 */
class ConstrainedMajorizationLayout {
public:
    ConstrainedMajorizationLayout(
        vector<Rectangle*>& rs,
        vector<Edge> const & es,
        RootCluster* clusterHierarchy,
        double const idealLength,
        std::valarray<double> const * eweights=NULL,
        TestConvergence& done=defaultTest,
        PreIteration* preIteration=NULL);
    /**
     * Horizontal alignment constraints
     */
    void setXConstraints(CompoundConstraints* ccsx) {
        constrainedLayout = true;
        this->ccsx=ccsx;
    }
    /**
     * Vertical alignment constraints
     */
    void setYConstraints(CompoundConstraints* ccsy) {
        constrainedLayout = true;
        this->ccsy=ccsy;
    }
    /**
     * These lists will have info about unsatisfiable constraints
     * after each iteration of constrained layout
     */
    void setUnsatisfiableConstraintInfo(
            UnsatisfiableConstraintInfos *unsatisfiableX,
            UnsatisfiableConstraintInfos *unsatisfiableY) {
        this->unsatisfiableX = unsatisfiableX;
        this->unsatisfiableY = unsatisfiableY;
    }
    /**
     * sticky nodes causes nodes to spring back to (startX,startY) when unconstrained
     */
    void setStickyNodes(const double stickyWeight, 
            valarray<double> const & startX,
            valarray<double> const & startY);

    /**
     * scaling speeds up the solver by conditioning the quadratic terms matrix.
     */
    void setScaling(bool scaling) {
        this->scaling=scaling;
    }
    /**
     * says that the Mosek optimisation library should be used to solve the quadratic
     * programs rather than the libvpsc solver
     */
    void setExternalSolver(bool externalSolver) {
        this->externalSolver=externalSolver;
    }
    /**
     * At each iteration of layout, generate constraints to avoid overlaps.
     * If bool horizontal is true, all overlaps will be resolved horizontally, otherwise
     * some overlaps will be left to be resolved vertically where doing so 
     * leads to less displacement
     */
    void setAvoidOverlaps(bool horizontal = false) {
        constrainedLayout = true;
        this->avoidOverlaps = horizontal?Horizontal:Both;
    }
    /**
     * Add constraints to prevent clusters overlapping
     */
    void setNonOverlappingClusters() {
        constrainedLayout = true;
        nonOverlappingClusters = true;
    }
    /**
     * For the specified edges (with routings), generate dummy vars and constraints
     * to try and straighten them.
     * bendWeight controls how hard we try to straighten existing bends
     * potBendWeight controls how much we try to keep straight edges straight
     */
    void setStraightenEdges(vector<straightener::Edge*>* straightenEdges, 
            double bendWeight = 0.01, double potBendWeight = 0.1,
            bool xSkipping = true) {
        for(vector<straightener::Edge*>::const_iterator e=straightenEdges->begin();
                e!=straightenEdges->end();e++) {
            (*e)->rerouteAround(boundingBoxes);
        }
        constrainedLayout = true;
        this->xSkipping = xSkipping;
        this->straightenEdges = straightenEdges;
        this->bendWeight = bendWeight;
        this->potBendWeight = potBendWeight;
    }
    /** update position of bounding boxes
     */
    void moveBoundingBoxes() {
        for(unsigned i=0;i<n;i++) {
            boundingBoxes[i]->moveCentre(X[i],Y[i]);
        }
    }

    ~ConstrainedMajorizationLayout() {
        if(constrainedLayout) {
            delete gpX;
            delete gpY;
        }
    }
    /**
     * run the layout algorithm in either the x-dim the y-dim or both
     */
    void run(bool x=true, bool y=true);
    void straighten(vector<straightener::Edge*>&, Dim);
    void setConstrainedLayout(bool c) {
        constrainedLayout=c;
    }
private:
    double euclidean_distance(unsigned i, unsigned j) {
        return sqrt(
            (X[i] - X[j]) * (X[i] - X[j]) +
            (Y[i] - Y[j]) * (Y[i] - Y[j]));
    }
    double compute_stress(valarray<double> const & Dij);
    void majorize(valarray<double> const & Dij,GradientProjection* gp, valarray<double>& coords, valarray<double> const & startCoords);
    void newton(valarray<double> const & Dij,GradientProjection* gp, valarray<double>& coords, valarray<double> const & startCoords);
    unsigned n; //!< number of nodes
    //valarray<double> degrees;
    valarray<double> lap2; //!< graph laplacian
    valarray<double> Q; //!< quadratic terms matrix used in computations
    valarray<double> Dij; //!< all pairs shortest path distances
    double tol; //!< convergence tolerance
    TestConvergence& done; //!< functor used to determine if layout is finished
    PreIteration* preIteration; //!< client can use this to create locks on nodes
    vector<Rectangle*> boundingBoxes; //!< node bounding boxes
    /** stickyNodes controls whether nodes are attracted to their starting
     * positions (at time of ConstrainedMajorizationLayout instantiation)
     * stored in startX, startY
     */
    valarray<double> X, Y;
    bool stickyNodes;
    double stickyWeight;
    valarray<double> startX;
    valarray<double> startY;
    double edge_length;
    bool constrainedLayout;
    bool nonOverlappingClusters;
    /**
     * A cluster is a set of nodes that are somehow semantically grouped
     * and should therefore be kept together a bit more tightly than, and
     * preferably without overlapping, the rest of the graph.
     *
     * We achieve this by augmenting the L matrix with stronger attractive
     * forces between all members of a cluster (other than the root)
     * and by maintaining a (preferably convex) hull around those 
     * constituents which, using constraints and dummy variables, is 
     * prevented from overlapping other parts of the graph.
     *
     * Clusters are defined over the graph in a hierarchy starting with
     * a single root cluster.
     *
     * Need to:
     *  - augment Lap matrix with intra cluster forces
     *  - compute convex hull of each cluster
     *  - from convex hull generate "StraightenEdges"
     */
    RootCluster *clusterHierarchy;
    straightener::LinearConstraints *linearConstraints;
    GradientProjection *gpX, *gpY;
    CompoundConstraints *ccsx, *ccsy;
    UnsatisfiableConstraintInfos *unsatisfiableX, *unsatisfiableY;
    NonOverlapConstraints avoidOverlaps;
    vector<straightener::Edge*>* straightenEdges;
    
    double bendWeight, potBendWeight;
    /** determines whether we should leave some overlaps to be resolved
     * vertically when generating straightening constraints in the x-dim
     */
    bool xSkipping;
    /** when using the gradient projection optimisation method, the following controls
     * whether the problem should be preconditioned by affine scaling
     */
    bool scaling;
    /** if the Mosek quadratic programming environment is available it may be used
       to solve each iteration of stress majorization... slow but useful for testing */
    bool externalSolver;
    bool majorization;
};

Rectangle bounds(vector<Rectangle*>& rs);

class Projection {
public:
    Projection(const unsigned n, CompoundConstraints * ccs, FixedList const &fixed);
    ~Projection();
    void solve(Variables &lvs, std::set<vpsc::Constraint*> &lcs, valarray<double> &X);
    void solve(Variables &lvs, std::set<vpsc::Constraint*> &lcs);
    Variables vars;
    Constraints gcs;
    CompoundConstraints* ccs;
private:
    unsigned n;
    FixedList const &fixed;
};

class ConstrainedFDLayout {
public:
    ConstrainedFDLayout(
        vector<Rectangle*>& rs,
        vector<Edge> const & es,
        RootCluster* clusterHierarchy,
        double const idealLength,
        std::valarray<double> const * eweights=NULL,
        TestConvergence& done=defaultTest,
        PreIteration* preIteration=NULL);
    void run(bool x=true, bool y=true);
    /**
     * Horizontal alignment constraints
     */
    void setXConstraints(CompoundConstraints* ccsx) {
        if(ccsx->size()>0) {
            printf("setXConstraints\n");
            constrainedX=true;
            this->ccsx=ccsx;
        }
    }
    /**
     * Vertical alignment constraints
     */
    void setYConstraints(CompoundConstraints* ccsy) {
        if(ccsy->size()>0) {
            printf("setYConstraints\n");
            constrainedY=true;
            this->ccsy=ccsy;
        }
    }
    void setAvoidOverlaps(bool avoidOverlaps) {
        if(avoidOverlaps) {
            printf("setAvoidOverlaps\n");
            constrainedX=constrainedY=true;
        }
        this->avoidOverlaps=avoidOverlaps;
    }
    void setTopology(vector<topology::Node*>* tnodes, vector<topology::Edge*>* routes) {
        printf("set Topology Constriants\n");
        topologyNodes=tnodes;
        topologyRoutes=routes;
        constrainedX=constrainedY=true;
    }
    ~ConstrainedFDLayout() {
        for(unsigned i=0;i<n;i++) {
            delete [] G[i];
            delete [] D[i];
        }
        delete [] G;
        delete [] D;
    }
    valarray<double> dummyNodesX, dummyNodesY;
    double computeStress() const;
private:
    unsigned n; // number of nodes
    valarray<double> X, Y;
    vector<Rectangle*> boundingBoxes;
    double applyForcesAndConstraints(const Dim dim,const double oldStress,const bool firstPass);
    double computeStepSize(SparseMatrix const & H, valarray<double> const & g,
            valarray<double> const & d) const;
    double applyDescentVector(
            valarray<double> const & d,
            valarray<double> const & oldCoords,
            valarray<double> &coords, 
            const double oldStress, 
            double stepsize
            /*,topology::TopologyConstraints *s=NULL*/) const;
    void move();
    void computeForces(const Dim dim, SparseMap &H, valarray<double> &g);
    vector<vector<unsigned> > neighbours;
    vector<vector<double> > neighbourLengths;
    TestConvergence& done;
    PreIteration* preIteration;
    FixedList fixed;
    double avgLength;
    double fc,fs;
    bool constrainedX, constrainedY;
    CompoundConstraints *ccsx, *ccsy;
    bool avoidOverlaps;
    std::auto_ptr<Projection> px, py;
    double** D;
    unsigned** G;
    vector<topology::Node*>* topologyNodes;
    vector<topology::Edge*>* topologyRoutes;
};

}
#endif				// COLA_H

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
