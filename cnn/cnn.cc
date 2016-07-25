#include "cnn/cnn.h"
#include "cnn/exec.h"
#include "cnn/nodes.h"
#include "cnn/param-nodes.h"
#include "cnn/aligned-mem-pool.h"
#include "cnn/cnn-helper.h"
#include "cnn/expr.h"

using namespace std;

namespace cnn {

float* kSCALAR_MINUSONE;
float* kSCALAR_ONE;
float* kSCALAR_ZERO;
int n_hgs = 0;

Node::~Node() {}
size_t Node::aux_storage_size() const { return 0; }

// perform the forward/backward passes in one or multiple calls
// TODO: This is a lot of code for something simple. Can it be shortened?
void Node::forward(const std::vector<const Tensor*>& xs,
                   Tensor& fx) const {
  if(this->supports_multibatch() || fx.d.batch_elems() == 1) {
    forward_impl(xs, fx);
  } else {
    for(unsigned b = 0; b < fx.d.batch_elems(); ++b) {
      std::vector<Tensor>  xs_elems;
      std::vector<const Tensor*> xs_ptrs;
      for(size_t i = 0; i < xs.size(); ++i) xs_elems.push_back(xs[i]->batch_elem(b));
      for(size_t i = 0; i < xs.size(); ++i) xs_ptrs.push_back(&xs_elems[i]);
      Tensor fx_elem(fx.batch_elem(b));
      forward_impl(xs_ptrs, fx_elem);
    }
  }
}
void Node::backward(const std::vector<const Tensor*>& xs,
                    const Tensor& fx,
                    const Tensor& dEdf,
                    unsigned i,
                    Tensor& dEdxi) const {
  if(this->supports_multibatch() || fx.d.batch_elems() == 1) {
    backward_impl(xs, fx, dEdf, i, dEdxi);
  } else {
    for(unsigned b = 0; b < fx.d.batch_elems(); ++b) {
      std::vector<Tensor>  xs_elems;
      std::vector<const Tensor*> xs_ptrs;
      for(size_t i = 0; i < xs.size(); ++i) xs_elems.push_back(xs[i]->batch_elem(b));
      for(size_t i = 0; i < xs.size(); ++i) xs_ptrs.push_back(&xs_elems[i]);
      Tensor fx_elem(fx.batch_elem(b));
      Tensor dEdf_elem(dEdf.batch_elem(b));
      Tensor dEdxi_elem(dEdxi.batch_elem(b));
      backward_impl(xs_ptrs, fx_elem, dEdf_elem, i, dEdxi_elem);
    }
  }
}

ComputationGraph::ComputationGraph() :
  ee(new SimpleExecutionEngine(*this)) {
  ++n_hgs;
  if (n_hgs > 1) {
    cerr << "Memory allocator assumes only a single ComputationGraph at a time.\n";
    throw std::runtime_error("Attempted to create >1 CG");
  }
}

ComputationGraph::~ComputationGraph() {
  this->clear();
  delete ee;
  --n_hgs;
}

void ComputationGraph::clear() {
  parameter_nodes.clear();
  for (auto n : nodes) delete n;
  nodes.clear();
}

CGCheckpoint ComputationGraph::_get_checkpoint() {
    CGCheckpoint p;
    p.device_mem_checkpoint = default_device->mark(this);
    p.node_idx = nodes.size();
    p.par_node_idx = parameter_nodes.size();
    return p;
}

void ComputationGraph::_revert(CGCheckpoint p) {
    default_device->revert(p.device_mem_checkpoint);
    // clear all nodes at position >= p.node_idx
    if (nodes.size() > p.node_idx) {
        nodes.resize(p.node_idx); // TODO verify deletion of nodes.
        ee->invalidate(p.node_idx-1); // clear precomputed forward values
    }
    // clear all parameter nodes at position >= p.par_node_idx
    if (parameter_nodes.size() > p.par_node_idx) {
        parameter_nodes.resize(p.par_node_idx);
    }
}

void ComputationGraph::checkpoint() {
    checkpoints.push_back(_get_checkpoint());
}

void ComputationGraph::revert() {
    if (checkpoints.size() == 0) return;
    _revert(checkpoints.back());
    checkpoints.pop_back();
}


VariableIndex ComputationGraph::add_input(real s) {
  VariableIndex new_node_index(nodes.size());
  nodes.push_back(new ScalarInputNode(s));
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_input(const real* ps) {
  VariableIndex new_node_index(nodes.size());
  nodes.push_back(new ScalarInputNode(ps));
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_input(const Dim& d, const vector<float>& pm) {
  VariableIndex new_node_index(nodes.size());
  nodes.push_back(new InputNode(d, pm));
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_input(const Dim& d, const vector<float>* pm) {
  VariableIndex new_node_index(nodes.size());
  nodes.push_back(new InputNode(d, pm));
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_input(const Dim& d, const vector<unsigned int>& ids, const vector<float>& data, float defdata) {
  VariableIndex new_node_index(nodes.size());
  nodes.push_back(new SparseInputNode(d, ids, data, defdata));
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_parameters(Parameter p) {
  VariableIndex new_node_index(nodes.size());
  ParameterNode* new_node = new ParameterNode(p);
  nodes.push_back(new_node);
  parameter_nodes.push_back(new_node_index);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_const_parameters(Parameter p) {
  VariableIndex new_node_index(nodes.size());
  ConstParameterNode* new_node = new ConstParameterNode(p);
  nodes.push_back(new_node);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_lookup(LookupParameter p, const unsigned* pindex) {
  VariableIndex new_node_index(nodes.size());
  LookupNode* new_node = new LookupNode(p, pindex);
  nodes.push_back(new_node);
  parameter_nodes.push_back(new_node_index);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_lookup(LookupParameter p, unsigned index) {
  VariableIndex new_node_index(nodes.size());
  LookupNode* new_node = new LookupNode(p, index);
  nodes.push_back(new_node);
  parameter_nodes.push_back(new_node_index);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_lookup(LookupParameter p, const std::vector<unsigned>& indices) {
  VariableIndex new_node_index(nodes.size());
  LookupNode* new_node = new LookupNode(p, indices);
  nodes.push_back(new_node);
  parameter_nodes.push_back(new_node_index);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_lookup(LookupParameter p, const std::vector<unsigned>* indices) {
  VariableIndex new_node_index(nodes.size());
  LookupNode* new_node = new LookupNode(p, indices);
  nodes.push_back(new_node);
  parameter_nodes.push_back(new_node_index);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}


VariableIndex ComputationGraph::add_const_lookup(LookupParameter p, const unsigned* pindex) {
  VariableIndex new_node_index(nodes.size());
  LookupNode* new_node = new LookupNode(p, pindex);
  // get rid of the following in favor of using parameter_nodes to see the needs_derivative
  // expression
  nodes.push_back(new_node);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_const_lookup(LookupParameter p, unsigned index) {
  VariableIndex new_node_index(nodes.size());
  LookupNode* new_node = new LookupNode(p, index);
  nodes.push_back(new_node);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_const_lookup(LookupParameter p, const std::vector<unsigned>& indices) {
  VariableIndex new_node_index(nodes.size());
  LookupNode* new_node = new LookupNode(p, indices);
  nodes.push_back(new_node);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

VariableIndex ComputationGraph::add_const_lookup(LookupParameter p, const std::vector<unsigned>* indices) {
  VariableIndex new_node_index(nodes.size());
  LookupNode* new_node = new LookupNode(p, indices);
  nodes.push_back(new_node);
  set_dim_for_new_node(new_node_index);
  return new_node_index;
}

// factory function should call this right after creating a new node object
// to set its dimensions properly
void ComputationGraph::set_dim_for_new_node(const VariableIndex& i) {
  Node* node = nodes[i];
  vector<Dim> xds(node->arity());
  unsigned ai = 0;
  for (VariableIndex arg : node->args) {
    xds[ai] = nodes[arg]->dim;
    ++ai;
  }
  node->dim = node->dim_forward(xds);
}

const Tensor& ComputationGraph::incremental_forward() { return ee->incremental_forward(); }
const Tensor& ComputationGraph::forward() { return ee->forward(); }
const Tensor& ComputationGraph::get_value(VariableIndex i) { return ee->get_value(i); }
const Tensor& ComputationGraph::get_value(const expr::Expression& e) { return this->get_value(e.i); }
void ComputationGraph::invalidate() { ee->invalidate(); }
void ComputationGraph::backward() { ee->backward(); }
void ComputationGraph::backward(VariableIndex i) { ee->backward(i); }

void ComputationGraph::print_graphviz() const {
  cerr << "digraph G {\n  rankdir=LR;\n  nodesep=.05;\n";
  unsigned nc = 0;
  for (auto node : nodes) {
    vector<string> var_names;
    for (auto arg : node->args)
      var_names.push_back(string("v") + to_string((unsigned)arg));
    cerr << "  N" << nc << " [label=\"v" << nc << " = "
         << node->as_string(var_names) << "\"];\n";
    for (auto arg : node->args)
      cerr << "  N" << ((unsigned)arg) << " -> N" << nc << ";\n";
    ++nc;
  }
  cerr << "}\n";
}

}  // namespace cnn

