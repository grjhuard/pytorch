#ifdef USE_CATCH

#define CATCH_CONFIG_MAIN
#include "catch_utils.hpp"

using Catch::StartsWith;

#else

#define CATCH_REQUIRE JIT_ASSERT

#endif

#include "torch/csrc/jit/assertions.h"
#include "torch/csrc/jit/fusers/interface.h"
#include "torch/csrc/jit/code_template.h"
#include "torch/csrc/jit/ir.h"
#include "torch/csrc/jit/attributes.h"
#include "torch/csrc/jit/interned_strings.h"
#include "torch/csrc/jit/interpreter.h"
#include "torch/csrc/jit/symbolic_variable.h"
#include "torch/csrc/jit/autodiff.h"
#include "torch/csrc/jit/dynamic_dag.h"
#include "torch/csrc/jit/tracer.h"
#include "torch/csrc/jit/passes/create_autodiff_subgraphs.h"
#include "torch/csrc/autograd/variable.h"
#include "torch/csrc/utils/hash.h"
#include "torch/csrc/jit/argument_spec.h"
#include "torch/csrc/jit/passes/shape_analysis.h"
#include "torch/csrc/jit/passes/requires_grad_analysis.h"
#include "torch/csrc/jit/passes/dead_code_elimination.h"
#include "torch/csrc/jit/passes/lower_grad_of.h"
#include "torch/csrc/jit/operator.h"
#include "torch/csrc/jit/custom_operator.h"
#include "torch/csrc/variable_tensor_functions.h"

#include "torch/csrc/autograd/variable.h"
#include "torch/csrc/autograd/engine.h"

#include "torch/csrc/jit/graph_executor.h"
#include "torch/csrc/jit/script/compiler.h"
#include "torch/csrc/jit/script/module.h"
#include "torch/csrc/jit/ivalue.h"

#include "onnx/onnx_pb.h"


#include <ATen/ATen.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace torch { namespace jit {

using Var = SymbolicVariable;

using namespace torch::autograd;

template<typename T>
static std::ostream & operator<<(std::ostream & out, const std::vector<T> & list) {
  size_t i = 0;
  out << "{";
  for(auto && e : list) {
    if(i++ > 0)
      out << ", ";
    out << e;
  }
  out << "}";
  return out;
}
static auto ct = CodeTemplate(R"(
int foo($args) {

    $bar
        $bar
    $a+$b
}
int commatest(int a${,stuff})
int notest(int a${,empty,})
)");
static auto ct_expect = R"(
int foo(hi, 8) {

    what
    on many
    lines...
    7
        what
        on many
        lines...
        7
    3+4
}
int commatest(int a, things..., others)
int notest(int a)
)";

static void codeTemplateTest() {
  {
    TemplateEnv e;
    e.s("hi","foo");
    e.v("what",{"is","this"});
    TemplateEnv c(e);
    c.s("hi","foo2");
    CATCH_REQUIRE(e.s("hi") == "foo");
    CATCH_REQUIRE(c.s("hi") == "foo2");
    CATCH_REQUIRE(e.v("what")[0] == "is");
  }

  {
    TemplateEnv e;
    e.v("args",{"hi","8"});
    e.v("bar",{"what\non many\nlines...","7"});
    e.s("a","3");
    e.s("b","4");
    e.v("stuff",{"things...","others"});
    e.v("empty",{});
    auto s = ct.format(e);
    //std::cout << "'" << s << "'\n";
    //std::cout << "'" << ct_expect << "'\n";
    CATCH_REQUIRE(s == ct_expect);
  }
}

Value * appendNewNode(NodeKind kind, Graph& graph, ArrayRef<Value*> inputs) {
  return graph.appendNode(graph.create(kind,inputs))->output();
}


static void fusionTests() {
  auto testSimple = [&] {
    Graph graph;
    Var i0 = Var::asNewInput(graph);
    Var i1 = Var::asNewInput(graph);
    auto o0 = i0 * i1;
    o0.addAsOutput();
    auto a = at::rand({3,4}, at::kCUDA);
    auto b = at::rand({4,3}, at::kCUDA).transpose(0,1);
    auto o = at::zeros({3,4}, at::kCUDA);
    auto outputs = debugLaunchGraph(graph, 0, {a,b});
    CATCH_REQUIRE(outputs.size() == 1);
    auto o2 = a*b;
    float max_diff = (o2 - outputs[0]).abs().max().item<double>();
    //std::cout << "max diff: " << max_diff << "\n";
    CATCH_REQUIRE(max_diff == 0);
  };
  testSimple();

  auto testOne = [&](int ti, int tj, int toi, int toj) {

    Graph graph;

    Var i0 = Var::asNewInput(graph);
    Var i1 = Var::asNewInput(graph);
    Var i2 = Var::asNewInput(graph);
    Var i3 = Var::asNewInput(graph);
    Var i4 = Var::asNewInput(graph);

    auto p22 =  i4.sigmoid();
    auto p20 = i3.sigmoid();
    auto p18 = i2.tanh();
    auto p16 = i1.sigmoid();
    auto p14 = p20 * i0;
    auto p11 = p22 * p18;
    auto o1 = p14 + p11;
    auto p5 = o1.tanh();
    auto o0 = p16 * p5;
    o0.addAsOutput();
    o1.addAsOutput();

    graph.lint();

    std::vector<at::Tensor> inputs;
    // We want to generate input/output tensors with dimension 128x128x32, but
    // with different internal strides.  To do this, we generate a tensor
    // with the "wrong" dimensions, and then use transpose to get an appropriately
    // sized view.
    for(size_t i = 0; i < graph.inputs().size(); i++) {
      std::vector<int64_t> dims = {128, 128, 32};
      std::swap(dims[ti],dims[tj]);
      inputs.push_back(at::rand(dims, at::kCUDA).transpose(ti, tj));
    }

    auto t22 = inputs[4].sigmoid();
    auto t20 = inputs[3].sigmoid();
    auto t18 = inputs[2].tanh();
    auto t16 = inputs[1].sigmoid();
    auto t14 = t20*inputs[0];
    auto t11 = t22*t18;
    auto out1 = t14+t11;
    auto t5 = out1.tanh();
    auto out0 = t16*t5;

    auto outputs = debugLaunchGraph(graph, 0, inputs);
    CATCH_REQUIRE(outputs.size() == graph.outputs().size());
    CATCH_REQUIRE(out0.is_same_size(outputs.front()));
    float max_diff = (outputs.front() - out0).abs().max().item<double>();
    CATCH_REQUIRE(max_diff < 1e-6);

  };
  testOne(0,0,0,0);
  testOne(0,1,0,0);
  testOne(1,2,0,0);
  testOne(0,2,0,0);

  testOne(0,0,0,1);
  testOne(0,1,1,2);
  testOne(1,2,0,2);


  auto createFusedConcat = [](Graph & graph, at::ArrayRef<Value*> inputs, int64_t dim) -> Value* {
    return graph.insertNode(graph.create(prim::FusedConcat, inputs)->i_(attr::dim, dim))->output();
  };

  auto testConcat = [&](int dim) {
    Graph graph;
    Var i0 = Var::asNewInput(graph);
    Var i1 = Var::asNewInput(graph);
    auto o0 = i0 * i1;
    o0.addAsOutput();
    Var(createFusedConcat(graph, {i0, o0}, dim)).addAsOutput();

    auto a = at::rand({3,4,5}, at::kCUDA);
    auto b = at::rand({4,3,5}, at::kCUDA).transpose(0,1);

    auto o_r = a*b;
    auto o2_r = at::cat({a, o_r}, dim);
    auto outputs = debugLaunchGraph(graph, 0, {a,b});
    CATCH_REQUIRE(outputs.size() == 2);

    float max_diff = (o_r - outputs[0]).abs().max().item<double>();
    CATCH_REQUIRE(max_diff == 0);
    float max_diff2 = (o2_r - outputs[1]).abs().max().item<double>();
    CATCH_REQUIRE(max_diff2 == 0);
  };
  testConcat(0);
  testConcat(1);
  testConcat(2);
}

struct Attr : public Attributes<Attr> {
};
void attributesTest() {
  auto one = attr::alpha;
  auto two = attr::device;
  auto three = attr::end;
  auto four = attr::perm;
  Attr attr;
  attr.f_(one,3.4)->i_(two,5)->s_(three,"what");
  CATCH_REQUIRE(attr.f(one) == 3.4);
  CATCH_REQUIRE(attr.s(three) == "what");
  CATCH_REQUIRE(attr.i(two) == 5);
  attr.s_(one,"no");
  CATCH_REQUIRE(attr.s(one) == "no");
  CATCH_REQUIRE(attr.hasAttribute(three));
  CATCH_REQUIRE(!attr.hasAttribute(four));
  attr.ss_(two, {"hi", "now"});
  CATCH_REQUIRE(attr.ss(two).at(1) == "now");

  Attr attr2;
  attr2.copyAttributes(attr);
  CATCH_REQUIRE(attr2.s(one) == "no");
  attr2.f_(one,5);
  CATCH_REQUIRE(attr.s(one) == "no");
  CATCH_REQUIRE(attr2.f(one) == 5);
}

void internedStringsTests () {

  CATCH_REQUIRE(prim::Param == Symbol::prim("Param"));
  CATCH_REQUIRE(prim::Return == Symbol::prim("Return"));
  CATCH_REQUIRE(prim::Return.toUnqualString() == std::string("Return"));
  CATCH_REQUIRE(prim::Return.toQualString() == std::string("prim::Return"));
  Symbol newsym = Symbol::aten("__NEW_SYMBOL");
  size_t symstart = newsym;
  CATCH_REQUIRE(newsym.toQualString() == std::string("aten::__NEW_SYMBOL"));
  // TODO: This test is a bit too close to the implementation details.
  CATCH_REQUIRE(Symbol::aten("What") == symstart+1);
  CATCH_REQUIRE(Symbol::aten("What2") == symstart+2);
  CATCH_REQUIRE(Symbol::aten("What") == symstart+1);
  CATCH_REQUIRE(Symbol::aten("What2") == symstart+2);
  CATCH_REQUIRE(Symbol(symstart+2).toUnqualString() == std::string("What2"));
}

void fromQualStringTests() {
  CATCH_REQUIRE(Symbol::fromQualString("prim::Param") == Symbol::prim("Param"));
  CATCH_REQUIRE(Symbol::fromQualString("aten::mm") == Symbol::aten("mm"));
  CATCH_REQUIRE(Symbol::fromQualString("onnx::LSTM") == Symbol::onnx("LSTM"));
  CATCH_REQUIRE(Symbol::fromQualString("attr::value") == Symbol::attr("value"));
  CATCH_REQUIRE(Symbol::fromQualString("scope::") == Symbol::scope(""));
  CATCH_REQUIRE(Symbol::fromQualString("::").toUnqualString() == std::string(""));
  CATCH_REQUIRE(Symbol::fromQualString("::").ns().toQualString() == std::string("namespaces::"));
  CATCH_REQUIRE(Symbol::fromQualString("new_ns::param").toUnqualString() == std::string("param"));
  CATCH_REQUIRE(Symbol::fromQualString("new_ns::param").ns().toUnqualString() == std::string("new_ns"));
  CATCH_REQUIRE(Symbol::fromQualString("new_ns::param").ns() == Symbol::fromQualString("namespaces::new_ns"));

  auto bad_inputs = {"scope", ":", ""};
  for (auto input : bad_inputs) {
    try {
      Symbol::fromQualString(input);
      CATCH_REQUIRE(0);
    } catch (std::runtime_error c) {
    }
  }
}

at::Tensor t_use(at::Tensor x) {
  return x;
}
at::Tensor t_def(at::Tensor x) {
  return x.t();
}

// given the difference of output vs expected tensor, check whether the
// difference is within a relative tolerance range. This is a standard way of
// matching tensor values upto certain precision
bool checkRtol(const at::Tensor& diff, const std::vector<at::Tensor> inputs) {
  double maxValue = 0.0;
  for (auto& tensor : inputs) {
    maxValue = fmax(tensor.abs().max().item<float>(), maxValue);
  }
  return diff.abs().max().item<float>() < 2e-6 * maxValue;
}
bool almostEqual(const at::Tensor & a, const at::Tensor & b) {
  return checkRtol(a - b,{a, b});
}

bool exactlyEqual(const at::Tensor & a, const at::Tensor & b) {
  return (a - b).abs().max().item<float>() == 0.f;
}

std::pair<at::Tensor, at::Tensor>
lstm(at::Tensor input,
      at::Tensor hx,
      at::Tensor cx,
      at::Tensor w_ih,
      at::Tensor w_hh) {
  auto gates = input.mm(t_use(w_ih)) + hx.mm(t_use(w_hh));

  auto chunked_gates = gates.chunk(4, 1);
  auto ingate     = chunked_gates[0];
  auto forgetgate = chunked_gates[1];
  auto cellgate = chunked_gates[2];
  auto outgate    = chunked_gates[3];

  ingate = ingate.sigmoid();
  outgate = outgate.sigmoid();
  cellgate = cellgate.tanh();
  forgetgate = forgetgate.sigmoid();

  auto cy = (forgetgate * cx) + (ingate * cellgate);
  auto hy = outgate * cy.tanh();

  return {hy, cy};
}

std::tuple<Var, Var> build_lstm_body(
  Graph & g,
  Var input,
  Var hx,
  Var cx,
  Var w_ih,
  Var w_hh) {
    auto gates = input.mm(w_ih);
    gates = gates + hx.mm(w_hh);
    auto outputs = gates.chunk(4, 1);
    auto ingate = outputs[0];
    auto forgetgate = outputs[1];
    auto cellgate = outputs[2];
    auto outgate = outputs[3];
    ingate = ingate.sigmoid();
    outgate = outgate.sigmoid();
    cellgate = cellgate.tanh();
    forgetgate = forgetgate.sigmoid();

    auto cy = forgetgate*cx;
    cy =  cy + ingate*cellgate;
    auto hy = outgate*cy.tanh();

    return std::make_tuple(hy,cy);
}

std::shared_ptr<Graph> build_lstm() {
  auto r = std::make_shared<Graph>();
  auto & g = *r;
  Value * input = g.addInput();
  Value * hx = g.addInput();
  Value * cx = g.addInput();
  Value * w_ih = g.addInput();
  Value * w_hh = g.addInput();

  Var hy;
  Var cy;
  std::tie(hy,cy) = build_lstm_body(g, input, hx, cx, w_ih, w_hh);

  hy.addAsOutput();
  cy.addAsOutput();
  g.lint();

  return r;
}

void run(InterpreterState & interp, const std::vector<at::Tensor> & inputs, std::vector<at::Tensor> & outputs) {
  std::vector<IValue> stack(inputs.begin(), inputs.end());
  interp.run(stack);
  outputs.clear();
  for(auto & ivalue : stack) {
    outputs.push_back(std::move(ivalue).toTensor());
  }
}

void interpTest() {
    constexpr int batch_size = 4;
    constexpr int input_size = 256;
    constexpr int seq_len = 32;

    int hidden_size = 2*input_size;

    auto input = at::randn({seq_len, batch_size, input_size}, at::kCUDA);
    auto hx    = at::randn({batch_size, hidden_size}, at::kCUDA);
    auto cx    = at::randn({batch_size, hidden_size}, at::kCUDA);
    auto w_ih  = t_def(at::randn({4 * hidden_size, input_size}, at::kCUDA));
    auto w_hh  = t_def(at::randn({4 * hidden_size, hidden_size}, at::kCUDA));

    auto lstm_g = build_lstm();
    Code lstm_function(lstm_g);
    std::vector<at::Tensor> outputs;
    InterpreterState lstm_interp(lstm_function);
    run(lstm_interp, {input[0], hx, cx, w_ih, w_hh}, outputs);
    std::tie(hx, cx) = lstm(input[0], hx, cx, w_ih, w_hh);

    //std::cout << almostEqual(outputs[0],hx) << "\n";
    CATCH_REQUIRE(exactlyEqual(outputs[0],hx));
    CATCH_REQUIRE(exactlyEqual(outputs[1],cx));
}

using var_meta_type = std::vector<int64_t>;
using var_meta_list = std::vector<var_meta_type>;
using test_fn_type = std::function<variable_list(const variable_list&)>;

struct ADTestSpec {
  ADTestSpec(const char *name, var_meta_list input_meta, test_fn_type test_fn)
    : name(name)
    , input_meta(input_meta)
    , test_fn(test_fn) {}

  variable_list operator()(const variable_list& inputs) const {
    return test_fn(inputs);
  };

  std::vector<Variable> make_vars() const {
    std::vector<Variable> out;
    for (const auto & m : input_meta) {
      out.emplace_back(autograd::make_variable(at::empty(m, at::TensorOptions()).normal_(), /*requires_grad=*/true));
    }
    return out;
  }

  const char *name;
  var_meta_list input_meta;
  test_fn_type test_fn;
};

variable_list get_grad_outputs(const variable_list& vars) {
  return fmap(vars, [](const Variable& v) -> Variable {
                      return at::randn(v.sizes(), v.options());
                    });
}

std::shared_ptr<Graph> trace(const ADTestSpec& test, const variable_list& vars_in) {
  std::shared_ptr<tracer::TracingState> state;
  Stack trace_stack_in;
  std::tie(state, trace_stack_in) = tracer::enter(fmap<IValue>(vars_in));
  variable_list trace_vars_in = fmap(trace_stack_in, [](const IValue& v) { return Variable(v.toTensor()); });
  auto trace_vars_out = test(trace_vars_in);
  tracer::exit(fmap<IValue>(trace_vars_out));
  return state->graph;
}

variable_list grad(const variable_list& outputs, const variable_list& inputs, const variable_list& grad_outputs) {
  static const auto get_edge = [](const Variable& v) { return v.gradient_edge(); };
  auto & engine = torch::autograd::Engine::get_default_engine();
  return engine.execute(fmap(outputs, get_edge), grad_outputs, true, false, fmap(inputs, get_edge));
}

void assertAllClose(const tensor_list& a, const tensor_list& b) {
  CATCH_REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CATCH_REQUIRE(a[i].is_same_size(b[i]));
    CATCH_REQUIRE(a[i].allclose(b[i]));
  }
}

std::pair<tensor_list, tensor_list> runGradient(Gradient& grad_spec,
                                                tensor_list& tensors_in,
                                                tensor_list& tensor_grads_in) {
  tensor_list tensors_out, tensor_grads_out;
  Code f_code{grad_spec.f},
      df_code{grad_spec.df};
  InterpreterState f_interpreter { f_code }, df_interpreter { df_code };

  run(f_interpreter, tensors_in, tensors_out);

  tensor_list df_inputs;
  df_inputs.insert(df_inputs.end(), tensor_grads_in.begin(), tensor_grads_in.end());
  for(auto offset : grad_spec.df_input_captured_inputs)
    df_inputs.push_back(tensors_in[offset]);
  for(auto offset : grad_spec.df_input_captured_outputs)
    df_inputs.push_back(tensors_out[offset]);
  run(df_interpreter, df_inputs, tensor_grads_out);

  // Outputs of f needs to be sliced
  tensors_out.erase(tensors_out.begin() + grad_spec.f_real_outputs, tensors_out.end());
  return std::make_pair(tensors_out, tensor_grads_out);
}

void testADFormulas() {
  static const auto unwrap = [](const Variable& v) { return v.data(); };

  using VL = variable_list;
  static const var_meta_list binary_pointwise = {{2, 3, 4, 5}, {2, 3, 4, 5}};
  static const var_meta_list unary_pointwise  = {{2, 3, 4, 5}};
  static const var_meta_list unary_pointwise_2d  = {{2, 3}};
  static const std::vector<ADTestSpec> ad_tests = {
    {"add",     binary_pointwise, [](const VL& v) -> VL { return {v[0] + v[1]}; }},
    {"sub",     binary_pointwise, [](const VL& v) -> VL { return {v[0] - v[1]}; }},
    {"mul",     binary_pointwise, [](const VL& v) -> VL { return {v[0] * v[1]}; }},
    {"sigmoid", unary_pointwise,  [](const VL& v) -> VL { return {v[0].sigmoid()}; }},
    {"tanh",    unary_pointwise,  [](const VL& v) -> VL { return {v[0].tanh()}; }},
    {"t",       unary_pointwise_2d,  [](const VL& v) -> VL { return {v[0].t()}; }},
    {"mm",      {{10, 12}, {12, 15}}, [](const VL& v) -> VL { return {v[0].mm(v[1])}; }},
    // TODO: enable once we'll be able to capture lists across forward-backward
    //{"chunk",   {{10, 12, 15}}, [](const VL& v) -> VL { return fmap<Variable>(v[0].chunk(4, 1)); }},
    //{"chunk",   {{10, 12, 15}}, [](const VL& v) -> VL { return fmap<Variable>(v[0].chunk(3, 2)); }},
    //{"split",   {{10, 12, 15}}, [](const VL& v) -> VL { return fmap<Variable>(v[0].split(4, 1)); }},
    //{"split",   {{10, 12, 15}}, [](const VL& v) -> VL { return fmap<Variable>(v[0].split(3, 2)); }},
  };

  for (const auto & test : ad_tests) {
    // Get reference values form autograd
    auto vars_in        = test.make_vars();
    auto vars_out       = test(vars_in);
    auto var_grads_in   = get_grad_outputs(vars_out);
    auto var_grads_out  = grad(vars_out, vars_in, var_grads_in);

    // Trace and differentiate the op
    auto graph = trace(test, vars_in);
    EliminateDeadCode(graph); // Tracing of some ops depends on the DCE trick
    auto grad_spec = differentiate(graph);
    LowerGradOf(*grad_spec.df);
    // Get outputs from the interpreter
    auto tensors_in                = fmap(vars_in, unwrap);
    auto tensor_grads_in           = fmap(var_grads_in, unwrap);
    tensor_list tensors_out, tensor_grads_out;
    std::tie(tensors_out, tensor_grads_out) = runGradient(grad_spec, tensors_in, tensor_grads_in);

    // Compare results
    auto expected_tensors_out      = fmap(vars_out, unwrap);
    auto expected_tensor_grads_out = fmap(var_grads_out, unwrap);
    assertAllClose(tensors_out,      expected_tensors_out);
    assertAllClose(tensor_grads_out, expected_tensor_grads_out);
  }
}

std::string toString(std::shared_ptr<Graph>& graph) {
  std::ostringstream s;
  s << *graph;
  return s.str();
}

void testDifferentiate(std::ostream & out) {
  auto graph = std::make_shared<Graph>();
  at::ScalarType s = at::ScalarType::Float;
  auto type = CompleteTensorType::create(s, -1, {2, 3, 4}, {12, 4, 1});

  // Build up a fake graph
  auto a = SymbolicVariable::asNewInput(*graph, type);
  auto b = SymbolicVariable::asNewInput(*graph, type);
  auto c = a * b * a + b;
  graph->registerOutput(c.value());

  auto grad_spec = differentiate(graph);
  std::vector<size_t> expected_captured_inputs = {0, 1};
  std::vector<size_t> expected_captured_outputs = {1};
  std::vector<size_t> expected_input_vjps = {0, 1};
  std::vector<size_t> expected_output_vjps = {0, 1};
  CATCH_REQUIRE(grad_spec.f_real_outputs == 1);
  CATCH_REQUIRE(grad_spec.df_input_captured_inputs == expected_captured_inputs);
  CATCH_REQUIRE(grad_spec.df_input_captured_outputs == expected_captured_outputs);
  CATCH_REQUIRE(grad_spec.df_input_vjps == expected_input_vjps);
  CATCH_REQUIRE(grad_spec.df_output_vjps == expected_output_vjps);
  out << "testDifferentiate\n";
  out << *grad_spec.f;
  out << *grad_spec.df;
  out << "\n";
}

void testDifferentiateWithRequiresGrad(std::ostream & out) {
  // Build up a fake graph
  auto graph = std::make_shared<Graph>();
  auto a = SymbolicVariable::asNewInput(*graph);
  auto b = SymbolicVariable::asNewInput(*graph);
  auto d = b * b + b;
  auto e = (d + a) * a + b;
  graph->registerOutput(d.value());
  graph->registerOutput(e.value());

  auto a_var = autograd::make_variable(at::CPU(at::kFloat).tensor(2, 2), true);
  auto b_var = autograd::make_variable(at::CPU(at::kFloat).tensor(2, 2), false);
  setInputTypes(*graph, ArgumentSpec(true, {a_var, b_var}, 2));
  PropagateInputShapes(*graph);
  PropagateRequiresGrad(graph);

  auto grad_spec = differentiate(graph);
  std::vector<size_t> expected_input_vjps = {1, 2};  // for e and %4 = (d + a)
  std::vector<size_t> expected_output_vjps = {0};    // only a requires grad
  CATCH_REQUIRE(grad_spec.f_real_outputs == 2);              // we need one temporary %4 = (d + a)
  CATCH_REQUIRE(grad_spec.df_input_captured_inputs == std::vector<size_t>({0}));
  CATCH_REQUIRE(grad_spec.df_input_captured_outputs == std::vector<size_t>({2}));
  CATCH_REQUIRE(grad_spec.df_input_vjps == expected_input_vjps);
  CATCH_REQUIRE(grad_spec.df_output_vjps == expected_output_vjps);
  out << "testDifferentiateWithRequiresGrad\n";
  out << *grad_spec.f;
  out << *grad_spec.df;
  out << "\n";
}

void testCreateAutodiffSubgraphs(std::ostream & out) {
  auto graph = build_lstm();
  CreateAutodiffSubgraphs(*graph, /*threshold=*/2);
  out << "testCreateAutodiffSubgraphs\n";
  out << *graph << "\n";
}

autograd::Variable var(at::Type & t, at::IntList sizes, bool requires_grad) {
  return autograd::make_variable(at::rand(sizes, t.options()), requires_grad);
}
autograd::Variable undef() {
  return autograd::Variable();
}

int device(const autograd::Variable & v) {
  return v.type().is_cuda() ? v.get_device() : -1;
}

bool isEqual(at::IntList lhs, at::IntList rhs) {
  return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

bool isEqual(const CompleteArgumentInfo & ti, const autograd::Variable & v) {
  CATCH_REQUIRE(ti.isTensor());
  if(!ti.defined())
    return ti.defined() == v.defined();
  return
    ti.device() == device(v) &&
    ti.requires_grad() == v.requires_grad() &&
    ti.type() == v.type().scalarType() &&
    isEqual(ti.sizes(), v.sizes()) &&
    isEqual(ti.strides(), v.strides());
}

// work around the fact that variable_tensor_list doesn't duplicate all
// of std::vector's constructors.
// most constructors are never used in the implementation, just in our tests.
Stack createStack(std::vector<at::Tensor> && list) {
  return Stack(std::make_move_iterator(list.begin()), std::make_move_iterator(list.end()));
}

void argumentSpecTest() {
  auto & CF = at::CPU(at::kFloat);
  auto & CD = at::CPU(at::kDouble);
  auto & GF = at::CUDA(at::kFloat);
  auto & GD = at::CUDA(at::kDouble);

  auto list = createStack({ var(CF, {1}, true), var(CD, {1, 2}, false) , var(GF, {}, true), var(GD, {4,5,6}, false), undef()});

  // make sure we have some non-standard strides
  list[1].toTensor().transpose_(0, 1);

  // same list but different backing values
  auto list2 = createStack({ var(CF, {1}, true), var(CD, {1, 2}, false) , var(GF, {}, true), var(GD, {4,5,6}, false), undef()});
  list2[1].toTensor().transpose_(0, 1);


  CompleteArgumentSpec a(true, list);
  CompleteArgumentSpec b(true, list);
  CATCH_REQUIRE(a.hashCode() == b.hashCode());

  CATCH_REQUIRE(a == b);
  CompleteArgumentSpec d(true, list2);
  CATCH_REQUIRE(d == a);
  CATCH_REQUIRE(d.hashCode() == a.hashCode());

  for(size_t i = 0; i < list.size(); ++i) {
    CATCH_REQUIRE(isEqual(a.at(i), list[i].toTensor()));
  }
  CompleteArgumentSpec no_grad(/*with_grad=*/false, list);
  CATCH_REQUIRE(no_grad != a);

  std::unordered_set<CompleteArgumentSpec> spec;
  spec.insert(a);
  CATCH_REQUIRE(spec.count(b) > 0);
  CATCH_REQUIRE(spec.count(no_grad) == 0);
  spec.insert(std::move(no_grad));
  CATCH_REQUIRE(spec.count(CompleteArgumentSpec(true,list)) == 1);

  list2[1].toTensor().transpose_(0,1);
  CompleteArgumentSpec c(true, list2); // same as list, except for one stride
  CATCH_REQUIRE(!(c == a));
  CATCH_REQUIRE(spec.count(c) == 0);

  Stack stack = { var(CF, {1,2}, true), 3, var(CF, {1,2}, true) };
  CompleteArgumentSpec with_const(true, stack);
  CATCH_REQUIRE(with_const.at(2).sizes().size() == 2);
}

void testGraphExecutor() {
  constexpr int batch_size = 4;
  constexpr int input_size = 256;

  int hidden_size = 2*input_size;

  auto v = [](at::Tensor t) { return autograd::make_variable(t, false); };

  auto input = at::randn({batch_size, input_size}, at::kCUDA);
  auto hx    = at::randn({batch_size, hidden_size}, at::kCUDA);
  auto cx    = at::randn({batch_size, hidden_size}, at::kCUDA);
  auto w_ih  = t_def(at::randn({4 * hidden_size, input_size}, at::kCUDA));
  auto w_hh  = t_def(at::randn({4 * hidden_size, hidden_size}, at::kCUDA));

  auto g = build_lstm();
  GraphExecutor executor(g);
  auto stack = createStack({v(input), v(hx), v(cx), v(w_ih), v(w_hh)});
  executor.run(stack);
  CATCH_REQUIRE(stack.size() == 2);
  at::Tensor r0, r1;
  std::tie(r0, r1) = lstm(input, hx, cx, w_ih, w_hh);
  CATCH_REQUIRE(almostEqual(Variable(stack[0].toTensor()).data(), r0));
  CATCH_REQUIRE(almostEqual(Variable(stack[1].toTensor()).data(), r1));
}

void testBlocks(std::ostream & out) {
  Graph g;
  auto a = Var::asNewInput(g, "a");
  auto b = Var::asNewInput(g, "b");
  auto c = a + b;
  auto r = g.appendNode(g.create(prim::If, {Var::asNewInput(g, "c").value()}));
  auto then_block = r->addBlock();
  auto else_block = r->addBlock();
  {
    WithInsertPoint guard(then_block);
    auto t = c + c;
    then_block->registerOutput(t.value());
  }
  {
    WithInsertPoint guard(else_block);
    auto  d = b + c;
    auto e = d + c;
    else_block->registerOutput(e.value());
  }
  g.registerOutput((Var(r->output()) + c).value());
  g.lint();
  out << "testBlocks\n" << g << "\n";
  r->eraseBlock(0);
  out << g << "\n";
  g.lint();
  // test recursive copy of blocks works
  auto g2 = g.copy();
  out << *g2 << "\n";
}


const static auto cf_examples = R"JIT(
  def if_test(a, b):
      # FIXME: use 0 instead of a.
      # c = 0
      c = a
      if bool(a < b):
        c = b
      else:
        c = a
      return c
  def if_one(a, b):
    c = b
    if bool(a < b):
      c = a
    return c
  def while_test(a, i):
    while bool(i < 3):
      a *= a
      i += 1
    return a
)JIT";
void testControlFlow() {
  script::Module cu;
  script::defineMethodsInModule(cu, cf_examples, script::nativeResolver, nullptr);
  auto run = [&](const std::string & name, std::vector<IValue> stack) {
    auto graph = cu.get_method(name).graph();
    Code code(graph);
    InterpreterState interp(code);
    interp.run(stack);
    return stack;
  };

  auto L = [](int64_t l) { return IValue(autograd::make_variable(scalar_to_tensor(at::Scalar(l)))); };
  auto V = [](IValue t) { return std::move(t).toTensor().item<int64_t>(); };
  auto run_binary = [&](const std::string & name, int64_t a, int64_t b) {
    return V(run(name, {L(a), L(b)})[0]);
  };
  CATCH_REQUIRE(2 == run_binary("if_test", 1, 2));
  CATCH_REQUIRE(3 == run_binary("if_test", 3, 2));
  CATCH_REQUIRE(2 == run_binary("if_one", 2, 3));
  CATCH_REQUIRE(2 == run_binary("if_one", 3, 2));
  CATCH_REQUIRE(256 == run_binary("while_test",2,0));
}

void testIValue() {
  Shared<IntList> foo = IntList::create({3, 4, 5});
  JIT_ASSERT(foo.use_count() == 1);
  IValue bar(foo);
  JIT_ASSERT(foo.use_count() == 2);
  auto baz = bar;
  JIT_ASSERT(foo.use_count() == 3);
  auto foo2 = std::move(bar);
  JIT_ASSERT(foo.use_count() == 3);
  JIT_ASSERT(foo2.isIntList());
  JIT_ASSERT(bar.isNone()); // NOLINT(bugprone-use-after-move)
  foo2 = IValue(4.0);
  JIT_ASSERT(foo2.isDouble());
  JIT_ASSERT(foo2.toDouble() == 4.0);
  JIT_ASSERT(foo.use_count() == 2);
  JIT_ASSERT(ArrayRef<int64_t>(baz.toIntList()->elements()).equals({3,4,5}));

  auto move_it = std::move(baz).toIntList();
  JIT_ASSERT(foo.use_count() == 2);
  JIT_ASSERT(baz.isNone()); // NOLINT(bugprone-use-after-move)
  IValue i(4);
  JIT_ASSERT(i.isInt() && i.toInt() == 4);
  IValue dlist(DoubleList::create({3.5}));
  JIT_ASSERT(
      dlist.isDoubleList() &&
      ArrayRef<double>(std::move(dlist).toDoubleList()->elements())
          .equals({3.5}));
  JIT_ASSERT(dlist.isNone()); // NOLINT(bugprone-use-after-move)
  dlist = IValue(DoubleList::create({3.4}));
  JIT_ASSERT(ArrayRef<double>(dlist.toDoubleList()->elements()).equals({3.4}));
  IValue the_list(Tuple::create({IValue(3.4), IValue(4), IValue(foo)}));
  JIT_ASSERT(foo.use_count() == 3);
  JIT_ASSERT(the_list.isTuple());
  auto first = std::move(the_list).toTuple()->elements().at(1);
  JIT_ASSERT(first.toInt() == 4);
  at::Tensor tv = at::rand({3,4});
  IValue ten(tv);
  JIT_ASSERT(tv.use_count() == 2);
  auto ten2 = ten;
  JIT_ASSERT(tv.use_count() == 3);
  JIT_ASSERT(ten2.toTensor().equal(ten.toTensor()));
  std::move(ten2).toTensor();
  JIT_ASSERT(tv.use_count() == 2);
}

void testProto() {
  ::ONNX_NAMESPACE::ModelProto proto;
  proto.set_producer_name("foo");
}

std::unique_ptr<detail::DynamicDAG<std::string>> newDynamicDAG() {
  return std::unique_ptr<detail::DynamicDAG<std::string>>(new detail::DynamicDAG<std::string>());
}

void testNewVertex() {
  auto graph = newDynamicDAG();
  JIT_ASSERT(graph->debugNumVertices() == 0);

  auto a = graph->newVertex("a");
  JIT_ASSERT(graph->debugNumVertices() == 1);
  JIT_ASSERT(a->ord == 0);
  JIT_ASSERT(a->data.size() == 1);
  JIT_ASSERT(a->data[0] == "a");
  JIT_ASSERT(a->in_edges().size() == 0);
  JIT_ASSERT(a->out_edges().size() == 0);

  auto b = graph->newVertex("b");
  auto c = graph->newVertex("c");
  JIT_ASSERT(graph->debugNumVertices() == 3);
  JIT_ASSERT(b->ord == 1);
  JIT_ASSERT(c->ord == 2);
}

void testAddEdgeBasic() {
  // a -> b -> c
  // \---------^
  auto graph = newDynamicDAG();
  auto a = graph->newVertex("a");
  auto b = graph->newVertex("b");
  auto c = graph->newVertex("c");
  graph->addEdge(a, b);
  graph->addEdge(b, c);
  graph->addEdge(a, c);
  JIT_ASSERT(a->in_edges().size() == 0);
  JIT_ASSERT(a->out_edges().size() == 2);
  JIT_ASSERT(a->out_edges().contains(b));
  JIT_ASSERT(a->out_edges().contains(c));

  JIT_ASSERT(b->in_edges().size() == 1);
  JIT_ASSERT(b->out_edges().size() == 1);
  JIT_ASSERT(b->in_edges().contains(a));
  JIT_ASSERT(b->out_edges().contains(c));

  JIT_ASSERT(c->in_edges().size() == 2);
  JIT_ASSERT(c->out_edges().size() == 0);
  JIT_ASSERT(c->in_edges().contains(a));
  JIT_ASSERT(c->in_edges().contains(b));
}

void testAddEdgeCycleDetection() {
  // a -> b -> c
  // ^---------/
  auto graph = newDynamicDAG();
  auto a = graph->newVertex("a");
  auto b = graph->newVertex("b");
  auto c = graph->newVertex("c");
  graph->addEdge(a, b);
  graph->addEdge(b, c);

  bool erred = false;
  try {
    graph->addEdge(c, a);
  } catch (c10::Error& err) {
    erred = true;
  }
  JIT_ASSERT(erred);
}

void testAddEdgeReordersBasic() {
  // a, b => b -> a
  auto graph = newDynamicDAG();
  auto a = graph->newVertex("a");
  auto b = graph->newVertex("b");
  JIT_ASSERT(a->ord == 0);
  JIT_ASSERT(b->ord == 1);

  graph->addEdge(b, a);
  JIT_ASSERT(a->ord == 1);
  JIT_ASSERT(b->ord == 0);
}

void testAddEdgeReordersComplicated() {
  // a -> b  c -> d with addEdge(d, b) ==>
  // c -> d -> a -> b
  auto graph = newDynamicDAG();
  auto a = graph->newVertex("a");
  auto b = graph->newVertex("b");
  auto c = graph->newVertex("c");
  auto d = graph->newVertex("d");
  graph->addEdge(a, b);
  graph->addEdge(c, d);
  JIT_ASSERT(a->ord == 0);
  JIT_ASSERT(b->ord == 1);
  JIT_ASSERT(c->ord == 2);
  JIT_ASSERT(d->ord == 3);

  graph->addEdge(d, a);
  JIT_ASSERT(c->ord == 0);
  JIT_ASSERT(d->ord == 1);
  JIT_ASSERT(a->ord == 2);
  JIT_ASSERT(b->ord == 3);

  JIT_ASSERT(c->in_edges().size() == 0);
  JIT_ASSERT(c->out_edges().size() == 1);
  JIT_ASSERT(c->out_edges().contains(d));

  JIT_ASSERT(d->in_edges().size() == 1);
  JIT_ASSERT(d->out_edges().size() == 1);
  JIT_ASSERT(d->in_edges().contains(c));
  JIT_ASSERT(d->out_edges().contains(a));

  JIT_ASSERT(a->in_edges().size() == 1);
  JIT_ASSERT(a->out_edges().size() == 1);
  JIT_ASSERT(a->in_edges().contains(d));
  JIT_ASSERT(a->out_edges().contains(b));

  JIT_ASSERT(b->in_edges().size() == 1);
  JIT_ASSERT(b->out_edges().size() == 0);
  JIT_ASSERT(b->in_edges().contains(a));
}

void testRemoveEdgeBasic() {
  // a -> b
  auto graph = newDynamicDAG();
  auto a = graph->newVertex("a");
  auto b = graph->newVertex("b");
  graph->addEdge(a, b);
  JIT_ASSERT(graph->debugNumVertices() == 2);

  graph->removeEdge(a, b);
  JIT_ASSERT(graph->debugNumVertices() == 2);
  JIT_ASSERT(a->out_edges().size() == 0);
  JIT_ASSERT(b->in_edges().size() == 0);
}

void testRemoveVertexBasic() {
  // a -> b
  auto graph = newDynamicDAG();
  auto a = graph->newVertex("a");
  auto b = graph->newVertex("b");
  auto c = graph->newVertex("c");
  graph->addEdge(a, b);
  graph->addEdge(b, c);
  JIT_ASSERT(graph->debugNumVertices() == 3);

  graph->removeVertex(b);
  JIT_ASSERT(graph->debugNumVertices() == 2);
  JIT_ASSERT(a->out_edges().size() == 0);
  JIT_ASSERT(c->in_edges().size() == 0);
}

void testContractEdgeBasic() {
  // a -> b -> c -> d
  auto graph = newDynamicDAG();
  auto a = graph->newVertex("a");
  auto b = graph->newVertex("b");
  auto c = graph->newVertex("c");
  auto d = graph->newVertex("d");
  graph->addEdge(a, b);
  graph->addEdge(b, c);
  graph->addEdge(c, d);

  graph->contractEdge(b, c);
  JIT_ASSERT(graph->debugNumVertices() == 3);
  JIT_ASSERT(a->out_edges().size() == 1);
  JIT_ASSERT(d->in_edges().size() == 1);
  JIT_ASSERT(*a->out_edges().begin() == *d->in_edges().begin());

  auto* contracted = *a->out_edges().begin();
  JIT_ASSERT(contracted->data.size() == 2);
  JIT_ASSERT(contracted->data[0] == "b");
  JIT_ASSERT(contracted->data[1] == "c");

  JIT_ASSERT(contracted->out_edges().size() == 1);
  JIT_ASSERT(contracted->in_edges().size() == 1);
  JIT_ASSERT(contracted->in_edges().contains(a));
  JIT_ASSERT(contracted->out_edges().contains(d));
}

void testContractEdgeCycleDetection() {
  // a -> b -> c
  // `---------^
  // contractEdge(a, c) will cause a cycle
  auto graph = newDynamicDAG();
  auto a = graph->newVertex("a");
  auto b = graph->newVertex("b");
  auto c = graph->newVertex("c");
  graph->addEdge(a, b);
  graph->addEdge(b, c);
  graph->addEdge(a, c);

  JIT_ASSERT(!graph->contractEdge(a, c));
}

void testDynamicDAG() {
  testNewVertex();
  testAddEdgeBasic();
  testAddEdgeCycleDetection();
  testAddEdgeReordersBasic();
  testAddEdgeReordersComplicated();
  testRemoveEdgeBasic();
  testRemoveVertexBasic();
  testContractEdgeBasic();
  testContractEdgeCycleDetection();
}

void testCustomOperators() {
  {
    RegisterOperators reg({createOperator(
        "foo::bar", [](double a, at::Tensor b) { return a + b; })});
    auto& ops = getAllOperatorsFor(Symbol::fromQualString("foo::bar"));
    CATCH_REQUIRE(ops.size() == 1);

    auto& op = ops.front();
    CATCH_REQUIRE(op->schema().name == "foo::bar");

    CATCH_REQUIRE(op->schema().arguments.size() == 2);
    CATCH_REQUIRE(op->schema().arguments[0].name == "_0");
    CATCH_REQUIRE(op->schema().arguments[0].type->kind() == TypeKind::FloatType);
    CATCH_REQUIRE(op->schema().arguments[1].name == "_1");
    CATCH_REQUIRE(op->schema().arguments[1].type->kind() == TypeKind::DynamicType);

    CATCH_REQUIRE(op->schema().returns[0].type->kind() == TypeKind::DynamicType);

    Stack stack;
    push(stack, 2.0f, autograd::make_variable(at::ones(5)));
    op->getOperation()(stack);
    at::Tensor output;
    pop(stack, output);

    CATCH_REQUIRE(output.allclose(autograd::make_variable(at::full(5, 3.0f))));
  }
  {
    RegisterOperators reg({createOperator(
        "foo::bar_with_schema(float a, Tensor b) -> Tensor",
        [](double a, at::Tensor b) { return a + b; })});

    auto& ops =
        getAllOperatorsFor(Symbol::fromQualString("foo::bar_with_schema"));
    CATCH_REQUIRE(ops.size() == 1);

    auto& op = ops.front();
    CATCH_REQUIRE(op->schema().name == "foo::bar_with_schema");

    CATCH_REQUIRE(op->schema().arguments.size() == 2);
    CATCH_REQUIRE(op->schema().arguments[0].name == "a");
    CATCH_REQUIRE(op->schema().arguments[0].type->kind() == TypeKind::FloatType);
    CATCH_REQUIRE(op->schema().arguments[1].name == "b");
    CATCH_REQUIRE(op->schema().arguments[1].type->kind() == TypeKind::DynamicType);

    CATCH_REQUIRE(op->schema().returns.size() == 1);
    CATCH_REQUIRE(op->schema().returns[0].type->kind() == TypeKind::DynamicType);

    Stack stack;
    push(stack, 2.0f, autograd::make_variable(at::ones(5)));
    op->getOperation()(stack);
    at::Tensor output;
    pop(stack, output);

    CATCH_REQUIRE(output.allclose(autograd::make_variable(at::full(5, 3.0f))));
  }
  {
    // Check that lists work well.
    RegisterOperators reg({createOperator(
        "foo::lists(int[] ints, float[] floats, Tensor[] tensors) -> float[]",
        [](const std::vector<int64_t>& ints,
           const std::vector<double>& floats,
           std::vector<at::Tensor> tensors) { return floats; })});

    auto& ops =
        getAllOperatorsFor(Symbol::fromQualString("foo::lists"));
    CATCH_REQUIRE(ops.size() == 1);

    auto& op = ops.front();
    CATCH_REQUIRE(op->schema().name == "foo::lists");

    CATCH_REQUIRE(op->schema().arguments.size() == 3);
    CATCH_REQUIRE(op->schema().arguments[0].name == "ints");
    CATCH_REQUIRE(op->schema().arguments[0].type->isSubtypeOf(ListType::ofInts()));
    CATCH_REQUIRE(op->schema().arguments[1].name == "floats");
    CATCH_REQUIRE(op->schema().arguments[1].type->isSubtypeOf(ListType::ofFloats()));
    CATCH_REQUIRE(op->schema().arguments[2].name == "tensors");
    CATCH_REQUIRE(op->schema().arguments[2].type->isSubtypeOf(ListType::ofTensors()));

    CATCH_REQUIRE(op->schema().returns.size() == 1);
    CATCH_REQUIRE(op->schema().returns[0].type->isSubtypeOf(ListType::ofFloats()));

    Stack stack;
    push(stack, std::vector<int64_t>{1, 2});
    push(stack, std::vector<double>{1.0, 2.0});
    push(stack, std::vector<at::Tensor>{autograd::make_variable(at::ones(5))});
    op->getOperation()(stack);
    std::vector<double> output;
    pop(stack, output);

    CATCH_REQUIRE(output.size() == 2);
    CATCH_REQUIRE(output[0] == 1.0);
    CATCH_REQUIRE(output[1] == 2.0);
  }
  {
    RegisterOperators reg(
        "foo::lists2(Tensor[] tensors) -> Tensor[]",
        [](std::vector<at::Tensor> tensors) { return tensors; });

    auto& ops =
        getAllOperatorsFor(Symbol::fromQualString("foo::lists2"));
    CATCH_REQUIRE(ops.size() == 1);

    auto& op = ops.front();
    CATCH_REQUIRE(op->schema().name == "foo::lists2");

    CATCH_REQUIRE(op->schema().arguments.size() == 1);
    CATCH_REQUIRE(op->schema().arguments[0].name == "tensors");
    CATCH_REQUIRE(op->schema().arguments[0].type->isSubtypeOf(ListType::ofTensors()));

    CATCH_REQUIRE(op->schema().returns.size() == 1);
    CATCH_REQUIRE(op->schema().returns[0].type->isSubtypeOf(ListType::ofTensors()));

    Stack stack;
    push(stack, std::vector<at::Tensor>{autograd::make_variable(at::ones(5))});
    op->getOperation()(stack);
    std::vector<at::Tensor> output;
    pop(stack, output);

    CATCH_REQUIRE(output.size() == 1);
    CATCH_REQUIRE(output[0].allclose(autograd::make_variable(at::ones(5))));
  }
  {
#ifdef USE_CATCH
    CATCH_REQUIRE_THROWS_WITH(
        createOperator(
            "foo::bar_with_bad_schema(Tensor a) -> Tensor",
            [](double a, at::Tensor b) { return a + b; }),
        StartsWith("Inferred 2 argument(s) for operator implementation, "
                   "but the provided schema specified 1 argument(s)."));
    CATCH_REQUIRE_THROWS_WITH(
        createOperator(
            "foo::bar_with_bad_schema(Tensor a) -> Tensor",
            [](double a) { return a; }),
        StartsWith("Inferred type for argument #0 was float, "
                   "but the provided schema specified type Dynamic "
                   "for the argument in that position"));
    CATCH_REQUIRE_THROWS_WITH(
        createOperator(
            "foo::bar_with_bad_schema(float a) -> (float, float)",
            [](double a) { return a; }),
        StartsWith("Inferred 1 return value(s) for operator implementation, "
                   "but the provided schema specified 2 return value(s)."));
    CATCH_REQUIRE_THROWS_WITH(
        createOperator(
            "foo::bar_with_bad_schema(float a) -> Tensor",
            [](double a) { return a; }),
        StartsWith("Inferred type for return value #0 was float, "
                   "but the provided schema specified type Dynamic "
                   "for the return value in that position"));
#endif // USE_CATCH
  }
  {
    auto op = createOperator(
        "traced::op(float a, Tensor b) -> Tensor",
        [](double a, at::Tensor b) { return a + b; });

    std::shared_ptr<tracer::TracingState> state;
    std::tie(state, std::ignore) = tracer::enter({});

    Stack stack;
    push(stack, 2.0f, autograd::make_variable(at::ones(5)));
    op.getOperation()(stack);
    at::Tensor output = autograd::make_variable(at::empty({}));
    pop(stack, output);

    tracer::exit({IValue(output)});

    std::string op_name("traced::op");
    bool contains_traced_op = false;
    for (const auto& node : state->graph->nodes()) {
      if (std::string(node->kind().toQualString()) == op_name) {
        contains_traced_op = true;
        break;
      }
    }
    CATCH_REQUIRE(contains_traced_op);
  }
  {
#ifdef USE_CATCH
    // vector<double> is not supported yet.
    auto op = createOperator(
        "traced::op(float[] f) -> int",
        [](const std::vector<double>& f) -> int64_t { return f.size(); });

    std::shared_ptr<tracer::TracingState> state;
    std::tie(state, std::ignore) = tracer::enter({});

    Stack stack;
    push(stack, std::vector<double>{1.0});

    CATCH_REQUIRE_THROWS_WITH(
        op.getOperation()(stack),
        StartsWith("Tracing float lists currently not supported!"));
#endif
  }
}

TORCH_API std::string runJITCPPTests() {
  std::stringstream out;
  testDynamicDAG();
  testIValue();
  testControlFlow();
  testGraphExecutor();
  testBlocks(out);
  testCreateAutodiffSubgraphs(out);
  testDifferentiate(out);
  testDifferentiateWithRequiresGrad(out);
  testADFormulas();
  interpTest();
  codeTemplateTest();
  fusionTests();
  attributesTest();
  internedStringsTests();
  fromQualStringTests();
  argumentSpecTest();
  testProto();
  testCustomOperators();
  return out.str();
}

#ifdef USE_CATCH

CATCH_TEST_CASE( "jit test CPU", "[cpu]" ) {

  std::stringstream out;
  CATCH_SECTION( "control flow" )
    testControlFlow();
  CATCH_SECTION( "blocks" )
    testBlocks(out);
  CATCH_SECTION( "create autodiff subgraphs" )
    testCreateAutodiffSubgraphs(out);
  CATCH_SECTION( "differentiate" )
    testDifferentiate(out);
  CATCH_SECTION( "differentiate with requires grad" )
    testDifferentiateWithRequiresGrad(out);
  CATCH_SECTION( "AD formulas" )
    testADFormulas();
  CATCH_SECTION( "code template" )
    codeTemplateTest();
  CATCH_SECTION( "attributes" )
    attributesTest();
  CATCH_SECTION( "interned strings" )
    internedStringsTests();
  CATCH_SECTION( "custom operators" )
    testCustomOperators();
}

CATCH_TEST_CASE( "jit test CUDA", "[cuda]" ) {

  CATCH_SECTION( "graph executor" )
    testGraphExecutor();
  CATCH_SECTION( "fusion" )
    fusionTests();
  CATCH_SECTION( "interp" )
    interpTest();
  CATCH_SECTION( "argument spec" )
    argumentSpecTest();
}

#endif

}}
