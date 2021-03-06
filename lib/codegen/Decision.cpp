#include "kllvm/codegen/Decision.h"
#include "kllvm/codegen/CreateTerm.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h" 

#include <iostream>

namespace kllvm {

static std::string BLOCK_STRUCT = "block";

FailNode FailNode::instance;

void Decision::operator()(DecisionNode *entry, llvm::StringMap<llvm::Value *> substitution) {
  if (entry == FailNode::get()) {
    llvm::BranchInst::Create(this->StuckBlock, this->CurrentBlock);
  } else {
    entry->codegen(this, substitution);
  }
}

std::set<std::string> DecisionNode::collectVars() {
  std::set<std::string> defs, vars;
  collectDefs(defs);
  collectUses(vars);
  for (std::string var : defs) {
    vars.erase(var);
  }
  return vars;
}

bool DecisionNode::beginNode(Decision *d, std::string name, llvm::StringMap<llvm::Value *> &substitution) {
  if (isCompleted()) {
    llvm::BranchInst::Create(cachedCode, d->CurrentBlock);
    for (std::string var : collectVars()) {
      phis.lookup(var)->addIncoming(substitution[var], d->CurrentBlock);
      substitution[var] = phis.lookup(var);
    }
    return true;
  }
  auto Block = llvm::BasicBlock::Create(d->Ctx,
      name,
      d->CurrentBlock->getParent());
  cachedCode = Block;
  llvm::BranchInst::Create(Block, d->CurrentBlock);
  for (std::string var : collectVars()) {
    auto Phi = llvm::PHINode::Create(substitution[var]->getType(), 1, "phi" + var, Block);
    Phi->addIncoming(substitution[var], d->CurrentBlock);
    phis[var] = Phi;
    substitution[var] = Phi;
  }
  d->CurrentBlock = Block;
  return false;
}

void SwitchNode::codegen(Decision *d, llvm::StringMap<llvm::Value *> substitution) {
  if (beginNode(d, "switch" + name, substitution)) {
    return;
  }
  llvm::Value *val = substitution.lookup(name);
  llvm::BasicBlock *_default = d->StuckBlock;
  const DecisionCase *defaultCase = nullptr;
  std::vector<std::pair<llvm::BasicBlock *, const DecisionCase *>> caseData;
  int idx = 0;
  bool isInt = false;
  for (auto &_case : cases) {
    auto child = _case.getChild();
    llvm::BasicBlock *CaseBlock;
    if (child == FailNode::get()) {
      CaseBlock = d->StuckBlock;
    } else {
      CaseBlock = llvm::BasicBlock::Create(d->Ctx, 
          name + "_case_" + std::to_string(idx++),
          d->CurrentBlock->getParent());
    }
    if (auto sym = _case.getConstructor()) {
      isInt = isInt || sym->getName() == "\\dv";
      caseData.push_back(std::make_pair(CaseBlock, &_case));
    } else {
      _default = CaseBlock;
      defaultCase = &_case;
    }
  }
  if (isCheckNull) {
    auto cast = new llvm::PtrToIntInst(val, llvm::Type::getInt64Ty(d->Ctx), "", d->CurrentBlock);
    auto cmp = new llvm::ICmpInst(*d->CurrentBlock, llvm::CmpInst::ICMP_NE, cast, llvm::ConstantExpr::getPtrToInt(llvm::ConstantPointerNull::get(llvm::dyn_cast<llvm::PointerType>(val->getType())), llvm::Type::getInt64Ty(d->Ctx)));
    val = cmp;
    isInt = true;
  }
  if (isInt) {
    auto _switch = llvm::SwitchInst::Create(val, _default, cases.size(), d->CurrentBlock);
    for (auto &_case : caseData) {
      _switch->addCase(llvm::ConstantInt::get(d->Ctx, _case.second->getLiteral()), _case.first);
    }
  } else { 
    if (caseData.size() == 0) {
      llvm::BranchInst::Create(_default, d->CurrentBlock);
    } else {
      llvm::Value *tagVal = d->getTag(val);
      auto _switch = llvm::SwitchInst::Create(tagVal, _default, caseData.size(), d->CurrentBlock);
      for (auto &_case : caseData) {
        _switch->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(d->Ctx), _case.second->getConstructor()->getTag()), _case.first); 
      }
    }
  }
  for (auto &entry : caseData) {
    auto &_case = *entry.second;
    if (entry.first == d->StuckBlock) {
      continue;
    }
    d->CurrentBlock = entry.first;
    if (!isInt) {
      int offset = 0;
      llvm::StructType *BlockType = getBlockType(d->Module, d->Definition, _case.getConstructor());
      llvm::BitCastInst *Cast = new llvm::BitCastInst(substitution.lookup(name), llvm::PointerType::getUnqual(BlockType), "", d->CurrentBlock);
      for (std::string binding : _case.getBindings()) {
        llvm::Value *ChildPtr = llvm::GetElementPtrInst::CreateInBounds(BlockType, Cast, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(d->Ctx), 0), llvm::ConstantInt::get(llvm::Type::getInt32Ty(d->Ctx), offset+2)}, "", d->CurrentBlock);
        switch (dynamic_cast<KOREObjectCompositeSort *>(_case.getConstructor()->getArguments()[offset++])->getCategory(d->Definition).cat) {
        case SortCategory::Map:
        case SortCategory::List:
        case SortCategory::Set:
          substitution[binding] = ChildPtr;
          break;
        default:
          substitution[binding] = new llvm::LoadInst(ChildPtr, binding, d->CurrentBlock);
          break;
        }
      }
    }
    _case.getChild()->codegen(d, substitution);
  }
  if (defaultCase) {
    if (_default != d->StuckBlock) {
      // process default also
      d->CurrentBlock = _default;
      defaultCase->getChild()->codegen(d, substitution);
    }
  }
  setCompleted();
}

void MakePatternNode::codegen(Decision *d, llvm::StringMap<llvm::Value *> substitution) {
  if (beginNode(d, "pattern" + name, substitution)) {
    return;
  }
  CreateTerm creator(substitution, d->Definition, d->CurrentBlock, d->Module, false);
  llvm::Value *val = creator(pattern).first;
  d->CurrentBlock = creator.getCurrentBlock();
  substitution[name] = val;
  child->codegen(d, substitution);
  setCompleted();
}

void FunctionNode::codegen(Decision *d, llvm::StringMap<llvm::Value *> substitution) {
  if (beginNode(d, "function" + name, substitution)) {
    return;
  }
  std::vector<llvm::Value *> args;
  std::vector<llvm::Type *> types;
  for (auto arg : bindings) {
    llvm::Value *val;
    if (arg.find_first_not_of("-0123456789") == std::string::npos) {
      val = llvm::ConstantInt::get(llvm::Type::getInt64Ty(d->Ctx), std::stoi(arg));
    } else {
      val = substitution.lookup(arg);
    }
    args.push_back(val);
    types.push_back(val->getType());
  }
  CreateTerm creator(substitution, d->Definition, d->CurrentBlock, d->Module, false);
  auto Call = creator.createFunctionCall(function, cat, args, function.substr(0, 5) == "hook_", false);
  Call->setName(name);
  substitution[name] = Call;
  child->codegen(d, substitution);
  setCompleted();
}


void LeafNode::codegen(Decision *d, llvm::StringMap<llvm::Value *> substitution) {
  if (beginNode(d, name, substitution)) {
    return;
  }
  std::vector<llvm::Value *> args;
  std::vector<llvm::Type *> types;
  for (auto arg : bindings) {
    auto val = substitution.lookup(arg);
    args.push_back(val);
    types.push_back(val->getType());
  }
  auto Call = llvm::CallInst::Create(d->Module->getOrInsertFunction(name, llvm::FunctionType::get(getValueType(d->Cat, d->Module), types, false)), args, "", d->CurrentBlock);
  Call->setCallingConv(llvm::CallingConv::Fast);
  llvm::ReturnInst::Create(d->Ctx, Call, d->CurrentBlock);
  setCompleted();
}

llvm::Value *Decision::getTag(llvm::Value *val) {
  return llvm::CallInst::Create(Module->getOrInsertFunction("getTag", llvm::Type::getInt32Ty(Ctx), getValueType({SortCategory::Symbol, 0}, Module)), val, "tag", CurrentBlock);
}

void makeEvalOrAnywhereFunction(KOREObjectSymbol *function, KOREDefinition *definition, llvm::Module *module, DecisionNode *dt, void (*addStuck)(llvm::BasicBlock*, llvm::Module*, KOREObjectSymbol *, llvm::StringMap<llvm::Value *>&, KOREDefinition *)) {
  auto returnSort = dynamic_cast<KOREObjectCompositeSort *>(function->getSort())->getCategory(definition);
  auto returnType = getValueType(returnSort, module);
  std::vector<llvm::Type *> args;
  std::vector<ValueType> cats;
  for (auto sort : function->getArguments()) {
    auto cat = dynamic_cast<KOREObjectCompositeSort *>(sort)->getCategory(definition);
    switch (cat.cat) {
    case SortCategory::Map:
    case SortCategory::List:
    case SortCategory::Set:
      args.push_back(llvm::PointerType::getUnqual(getValueType(cat, module)));
      cats.push_back(cat);
      break;
    default:
      args.push_back(getValueType(cat, module));
      cats.push_back(cat);
      break;
    }
  }
  llvm::FunctionType *funcType = llvm::FunctionType::get(returnType, args, false);
  std::ostringstream Out;
  function->print(Out, 0, false);
  std::string name = "eval_" + Out.str();
  llvm::Constant *func = module->getOrInsertFunction(name, funcType);
  llvm::Function *matchFunc = llvm::cast<llvm::Function>(func);
  matchFunc->setCallingConv(llvm::CallingConv::Fast);
  llvm::StringMap<llvm::Value *> subst;
  llvm::BasicBlock *block = llvm::BasicBlock::Create(module->getContext(), "entry", matchFunc);
  int i = 0;
  for (auto val = matchFunc->arg_begin(); val != matchFunc->arg_end(); ++val, ++i) {
    val->setName("_" + std::to_string(i+1));
    subst.insert({val->getName(), val});
  }
  llvm::BasicBlock *stuck = llvm::BasicBlock::Create(module->getContext(), "stuck", matchFunc);
  addStuck(stuck, module, function, subst, definition);

  Decision codegen(definition, block, stuck, module, returnSort);
  codegen(dt, subst);
}

void abortWhenStuck(llvm::BasicBlock *stuck, llvm::Module *module, KOREObjectSymbol *, llvm::StringMap<llvm::Value *> &, KOREDefinition *) {
  addAbort(stuck, module);
}

void makeEvalFunction(KOREObjectSymbol *function, KOREDefinition *definition, llvm::Module *module, DecisionNode *dt) {
  makeEvalOrAnywhereFunction(function, definition, module, dt, abortWhenStuck);
}

void addOwise(llvm::BasicBlock *stuck, llvm::Module *module, KOREObjectSymbol *symbol, llvm::StringMap<llvm::Value *> &subst, KOREDefinition *d) {
  CreateTerm creator = CreateTerm(subst, d, stuck, module, true);
  KOREObjectCompositePattern *pat = KOREObjectCompositePattern::Create(symbol);
  for (int i = 0; i < symbol->getArguments().size(); i++) {
     auto var = KOREObjectVariablePattern::Create("_" + std::to_string(i+1), symbol->getArguments()[i]);
     pat->addArgument(var);
  }
  llvm::Value *retval = creator(pat).first;
  auto returnSort = dynamic_cast<KOREObjectCompositeSort *>(symbol->getSort())->getCategory(d);
  auto returnType = getValueType(returnSort, module);
  if (retval->getType() == llvm::PointerType::getUnqual(returnType)) {
    retval = new llvm::LoadInst(retval, "", creator.getCurrentBlock());
  }
  llvm::ReturnInst::Create(module->getContext(), retval, creator.getCurrentBlock());
}

void makeAnywhereFunction(KOREObjectSymbol *function, KOREDefinition *definition, llvm::Module *module, DecisionNode *dt) {
  makeEvalOrAnywhereFunction(function, definition, module, dt, addOwise);
}

void makeStepFunction(KOREDefinition *definition, llvm::Module *module, DecisionNode *dt) {
  auto blockType = getValueType({SortCategory::Symbol, 0}, module);
  llvm::FunctionType *funcType = llvm::FunctionType::get(blockType, {blockType}, false);
  std::string name = "step";
  llvm::Constant *func = module->getOrInsertFunction(name, funcType);
  llvm::Function *matchFunc = llvm::cast<llvm::Function>(func);
  llvm::StringMap<llvm::Value *> subst;
  auto val = matchFunc->arg_begin();
  val->setName("_1");
  subst.insert({val->getName(), val});
  llvm::BasicBlock *block = llvm::BasicBlock::Create(module->getContext(), "entry", matchFunc);
  llvm::BasicBlock *stuck = llvm::BasicBlock::Create(module->getContext(), "stuck", matchFunc);
  llvm::ReturnInst::Create(module->getContext(), llvm::ConstantPointerNull::get(llvm::dyn_cast<llvm::PointerType>(blockType)), stuck);

  Decision codegen(definition, block, stuck, module, {SortCategory::Symbol, 0});
  codegen(dt, subst);
}


}
