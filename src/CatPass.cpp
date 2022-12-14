#include "llvm/ADT/Statistic.h"
#include "llvm/Pass.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Transforms/Utils/LoopPeel.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Use.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Transforms/Utils/LoopRotationUtils.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include <climits>
#include <concepts>
#include <set>
#include <list>
#include <iostream>
#include <map>
#include <unordered_set>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <queue>
using namespace llvm;
using namespace std;
namespace {
  struct CAT : public ModulePass {
    static char ID; 
    unordered_map<Value*,int>ssaVarOrder;//ssaVar contains cat_new cat_add cat_sub cat_set and new function
    unordered_map<int,Value*>orderSsaVar;
    unordered_map<Value*,BitVector>GEN,KILL,IN,OUT;
    set<StringRef>catFuncName;
    unordered_map<Value*,BitVector>mayAlias;
    unordered_map<int,BitVector>mustAlias;
    unordered_map<Value*,BitVector>ssaBit;
    const DataLayout *DL;
    Function* mainF;
    int totalCnt=0;
    bool isPause=true;
    int maxDep=0;
    bool canDelete=false;
    vector<bool>ssaVarDel;
    CAT() : ModulePass(ID) {}

    bool doInitialization (Module &M) override {
      mainF = M.getFunction("main");
      //errs()<<*mainF<<'\n'
      canDelete=getDetele();
      DL = &((&M)->getDataLayout());
      catFuncName.insert("CAT_new");
      catFuncName.insert("CAT_add");
      catFuncName.insert("CAT_sub");
      catFuncName.insert("CAT_set");
      return false;
    }
    void clear()
    {
      ssaVarOrder.clear();
      orderSsaVar.clear();
      GEN.clear();KILL.clear();IN.clear();OUT.clear();
      mustAlias.clear();
      ssaBit.clear();
      totalCnt=0;
    }
    void setCluster(Instruction* ins)
    {
      if(auto inst=dyn_cast<CallInst>(ins))
      {
        auto funcName=inst->getCalledFunction()->getName();
        //errs()<<"func_name:"<<funcName<<"\n";
        //errs()<<"set Cluster for ins"<<*ins<<"\n";
        //printVar();
        //errs()<<"halo!"<<"\n";
        if(funcName=="CAT_new")
        {
          //errs()<<"it is new!"<<"\n";
          mustAlias[ssaVarOrder[ins]]|=ssaBit[ins];
        }
        else if(catFuncName.count(funcName))
        {
          //errs()<<*ins<<"\n";
          auto op=dyn_cast<Instruction>(inst->getOperand(0));
          //if(funcName=="CAT_set"&&isa<SExtInst>(inst->getOperand(1)))return;
          if(isa<PHINode>(inst->getOperand(0)))//we need a queue to get everything of this instruction
          {
            auto phi=dyn_cast<PHINode>(inst->getOperand(0));
            queue<PHINode*>q;
            q.push(phi);
            while(!q.empty())
            {
              auto tmp=q.front();
              q.pop();
              for(uint i=0;i<tmp->getNumOperands();++i)
              {
                if(auto calVar=dyn_cast<CallInst>(tmp->getOperand(i)))
                {
                  if(calVar->getCalledFunction()->getName()=="CAT_new")
                  {
                    //errs()<<"it is a new "<<*calVar<<"\n";
                    if(ssaVarOrder.find(calVar)!=ssaVarOrder.end())mustAlias[ssaVarOrder[calVar]]|=ssaBit[inst];
                  }
                }
                else if(auto phiVar=dyn_cast<PHINode>(tmp->getOperand(i)))
                {
                  q.push(phiVar);
                }
              }
            }
          }
          else
          {
            //errs()<<"it is a variable"<<"\n";
            if(ssaVarOrder.find(op)!=ssaVarOrder.end())mustAlias[ssaVarOrder[op]]|=ssaBit[ins];//for new
          }
        }
        else if(ssaVarOrder.count(inst))//it is a new function
        {
          //errs()<<"new function!"<<*inst<<":\n";
          for(uint i=0;i<inst->getNumOperands();++i)
          {
            if(auto var=dyn_cast<CallInst>(inst->getOperand(i)))
            {
              if(ssaVarOrder.find(var)!=ssaVarOrder.end())
              {
                //errs()<<*var<<"\n";
                mustAlias[ssaVarOrder[var]]|=ssaBit[ins];
                mayAlias[var]|=ssaBit[ins];
              }
            }
          }
        }
      }
      else if(auto inst=dyn_cast<StoreInst>(ins))
      {
        auto var=dyn_cast<CallInst>(inst->getValueOperand());
        if(ssaVarOrder.find(var)!=ssaVarOrder.end())
        {
            
        }
      }
      //printVar();
    }
    void replaceVar(CallInst* bef,CallInst* aft)
    {
      int index=ssaVarOrder[bef];
      ssaVarOrder[aft]=index;
      ssaVarOrder.erase(bef);
      orderSsaVar[index]=aft;
      ssaBit[aft]=ssaBit[bef];
      ssaBit.erase(bef);
      bef=nullptr;
    }
    void setGenAndKill(Instruction* ins)//kill[ins]^=mustAlias[var]
    {
      //errs()<<"set gen and kill for inst "<<*ins<<"\n";
      BitVector gen(totalCnt,false),kill(totalCnt,false);
      if(isa<CallInst>(ins))
      {
        auto inst=dyn_cast<CallInst>(ins);
        auto funcName=inst->getCalledFunction()->getName();
        if(funcName=="CAT_new")
        {
          gen[ssaVarOrder[ins]]=true;
          kill^=mustAlias[ssaVarOrder[ins]];
          kill[ssaVarOrder[ins]]=false;
        }
        else if(catFuncName.count(funcName)||funcName=="CAT_get")
        {
          auto var=dyn_cast<Instruction>(inst->getOperand(0));
          if(catFuncName.count(funcName))gen[ssaVarOrder[ins]]=true;
          if(funcName=="CAT_set")
          {
            //errs()<<"operand is "<<*(inst->getOperand(1))<<"\n";
          }
          if(funcName=="CAT_set"&&(isa<SExtInst>(inst->getOperand(1))))
          {
            //errs()<<"Cannot propagate the set"<<"\n";
            goto setfail;
          }
          if(var&&isa<PHINode>(var))
          {
            queue<PHINode*>q;
            auto p=dyn_cast<PHINode>(var);
            q.push(p);
            while(!q.empty())
            {
              auto phi=q.front();
              q.pop();
              for(uint i=0;i<phi->getNumOperands();++i)
              {
                auto phiVar=phi->getOperand(i);
                if(auto calVar=dyn_cast<CallInst>(phiVar))
                {
                  //maybe add the add and sub situation
                  if(calVar->getCalledFunction()->getName()=="CAT_new")
                  {
                    //errs()<<"the var is "<<*calVar<<"\nbit is\n";
                    //printBit(mustAlias[ssaVarOrder[calVar]]);
                    kill|=mustAlias[ssaVarOrder[calVar]];
                    kill[ssaVarOrder[calVar]]=false;
                  }
                }
                else if(auto newPhi=dyn_cast<PHINode>(phiVar))
                {
                  q.push(newPhi);
                }
              }
            }
          }
          else if(catFuncName.count(funcName))
          {
            //errs()<<"set kill for "<<*inst<<"\n";
            //printBit(mustAlias[ssaVarOrder[var]]);
            if(ssaVarOrder.find(var)!=ssaVarOrder.end())kill|=mustAlias[ssaVarOrder[var]];
            kill[ssaVarOrder[ins]]=false;
          }
        }
        else if(ssaVarOrder.find(ins)!=ssaVarOrder.end())//for new instruction like p
        {
          gen[ssaVarOrder[ins]]=true;
          kill=getModify(dyn_cast<CallInst>(ins));
        }
      }
      else if(auto inst=dyn_cast<StoreInst>(ins))
      {
        kill|=mustAlias[ssaVarOrder[dyn_cast<CallInst>(inst->getValueOperand())]];
        kill[ssaVarOrder[inst]]=false;
      }
      else if(auto inst=dyn_cast<LoadInst>(ins))
      {
        //errs()<<"it is a load inst:"<<*ins<<"\n"; 
        //errs()<<*(inst->getOperand(0))<<"\n";
      }
      setfail:
      ;
      GEN[ins]=gen;
      KILL[ins]=kill; 
      
    }
    void setInAndOut(Function& F)
    {
      queue<Instruction*>q;
      for(auto& I:instructions(F))
      {
        setGenAndKill(&I);
        q.push(&I);
      }
      while(!q.empty())
      {
        auto tmp=q.front();
        //errs()<<"set in set for instruction "<<*tmp<<"\n";
        q.pop();
        auto bb=tmp->getParent();
        auto gen=GEN[tmp],kill=KILL[tmp],in=BitVector(totalCnt,false),oldOut=OUT[tmp];
        if(tmp==&(bb->front()))//first instruction
        {
          //errs()<<"This instruction is a front"<<"\n";
          for(auto pa:predecessors(bb))
          {
            in|=OUT[pa->getTerminator()];
            //errs()<<"The previous instruction is"<<*(pa->getTerminator())<<"\n";
          }
        }
        else
        {
          //errs()<<"This instruction is a inner ins"<<"\n";
          in|=OUT[tmp->getPrevNode()];
          //errs()<<"The previous instruction is"<<*(tmp->getPrevNode());
        }
        //errs()<<"The in set is"<<"\n";
        //printBit(in);
        IN[tmp]=in;
        OUT[tmp]=in;
        OUT[tmp]&=kill.flip();
        OUT[tmp]|=gen;
        //errs()<<"The out set is"<<"\n";
        //printBit(OUT[tmp]);
        //errs()<<"The kill set is"<<"\n";
        //printBit(KILL[tmp]);
        //errs()<<"The gen set is"<<"\n";
        //printBit(GEN[tmp]);
        if(oldOut!=OUT[tmp])
        {
          //errs()<<"This instruction need recalculate"<<"\n";
          if(bb->getTerminator()==tmp)
          {
            for(auto son:successors(bb))
            {
              //errs()<<"it pushed "<<*(&(son->front()))<<"\n";
              q.push(&(son->front()));
            }
          }
          else 
          {
            //errs()<<"it pushed "<<*(tmp->getNextNode())<<"\n";
            q.push(tmp->getNextNode());
          }
        }
      }
    }
    void addVar(Value* I)
    {
      ssaVarOrder[I]=totalCnt;
      orderSsaVar[totalCnt++]=I;
    }
    void init(Function &F)
    {
      //clear();
      //add all value
      for(auto& I:instructions(F))
      {
        if(isa<CallInst>(&I))
        {
          auto inst=dyn_cast<CallInst>(&I);
          auto funcName=inst->getCalledFunction()->getName();
          if(catFuncName.count(funcName))//may delete in the future about(isa<StoreInst>(&I))
          {
            addVar(&I);
          }
        }
        else if(isa<StoreInst>(&I))addVar(&I);
        else if(isa<LoadInst>(&I))
        {

        }
      }
      function<bool(CallInst*)>check=[&](CallInst* inst)
      {
        bool isVariable=false;
        if(inst->getCalledFunction()->getName()=="CAT_get")return isVariable;
        for(uint i=0;i<inst->getNumOperands();++i)
        {
          if(auto var=dyn_cast<CallInst>(inst->getOperand(i)))
          {
            isVariable=true;
            break;
          }
        }
        if(isVariable)addVar(inst);
        return isVariable;
      };
      //get bit
      for(auto& I:instructions(F))
      {
        if(isa<CallInst>(&I))
        {
          auto inst=dyn_cast<CallInst>(&I);
          auto funcName=inst->getCalledFunction()->getName();
          if(catFuncName.count(funcName)||check(inst))
          {
            auto tmp=BitVector(totalCnt,false);
            tmp[ssaVarOrder[&I]]=true;
            ssaBit[&I]=tmp;
          }
        }
        else if(isa<StoreInst>(&I))
        {
          auto tmp=BitVector(totalCnt,false);
          tmp[ssaVarOrder[&I]]=true;
          ssaBit[&I]=tmp;
        }
      }
      for(auto& I:instructions(F))
      {
        if(isa<CallInst>(&I)||isa<StoreInst>(&I)||isa<LoadInst>(&I))
        {
          //errs()<<I<<"\n";
          setCluster(&I);
          //printVar();
        }
      }
      setInAndOut(F);
    }
    void memoryAlias(CallInst* inst)
    {
      unordered_set<CallInst*>delList;
      auto op=inst->getOperand(0);
      while(inst->getPrevNode()&&isa<CallInst>(inst->getPrevNode())&&catFuncName.count((dyn_cast<CallInst>(inst->getPrevNode()))->getCalledFunction()->getName()))
      {
        auto pre=dyn_cast<CallInst>(inst->getPrevNode());
        if(pre->getOperand(0)==op)
        {
          ssaVarDel[ssaVarOrder[pre]]=true;
          delList.insert(pre);
        }
        inst=pre;
      }
      for(auto inst:delList)inst->eraseFromParent();
    }
    void constantPropagation(Function& F)
    {
      queue<CallInst*>q;
      unordered_set<CallInst*>delList;
      ssaVarDel=vector<bool>(totalCnt,false);
      for(auto& I:instructions(F))
      {
        if(auto inst=dyn_cast<CallInst>(&I))
        {
          auto funcName=inst->getCalledFunction()->getName();
          if((catFuncName.count(funcName)&&funcName!="CAT_new")||funcName=="CAT_get")
          {
            q.push(inst);
          }
          if(funcName=="CAT_new"&&inst->getNumUses()==0&&canDelete)
          {
            delList.insert(inst);
          }
        }
      }
      while(!q.empty())
      {
        auto inst=q.front();
        q.pop();
        aliasOptimization(F);
        //errs()<<"do propagation for ins "<<*inst<<"\n";
        auto funcName=inst->getCalledFunction()->getName();
        ConstantInt* value=nullptr;
        //bool isDelete=false;
        if((value=constantFolding(inst)))
        {
          if(funcName=="CAT_add"||funcName=="CAT_sub")
          {
            //isDelete=true;
            IRBuilder<> builder(inst);
            CallInst* newInst=builder.CreateCall(F.getParent()->getFunction("CAT_set"),{inst->getOperand(0),value});
            newInst->setTailCall();
            replaceVar(inst,newInst);
            inst->eraseFromParent();
            q.push(newInst);
          }
          if(funcName=="CAT_get")inst->replaceAllUsesWith(value);
          if((funcName=="CAT_get"||funcName=="CAT_set")&&!deadCodeCheck(inst))
          {
            delList.insert(inst);
            if(funcName=="CAT_set")ssaVarDel[ssaVarOrder[inst]]=true;
          }
        }
        for(auto& I:instructions(F))
        {
          if(auto inst=dyn_cast<CallInst>(&I))
          {
            if(funcName=="CAT_new"&&inst->getNumUses()==0)
            {
              delList.insert(inst);
            }
          } 
        }
        /*
        if((funcName=="CAT_set"||funcName=="CAT_get"||((funcName=="CAT_add"||funcName=="CAT_sub")&&isDelete==false))&&!deadCodeCheck(inst))
        {
          delList.insert(inst);
        }
        */
        //errs()<<F<<"\n";
        //printMustAlias();
        //errs()<<"\n\n\n";
      }
      for(auto &I:delList)
      {
        //errs()<<"removed "<<*I<<"\n";
        I->eraseFromParent();
      }
      for(auto& I:instructions(F))
      {
        if(auto calIns=dyn_cast<CallInst>(&I))
        {
          if(calIns->getCalledFunction()->getName()=="CAT_set")memoryAlias(calIns);
        }
      }
    }
    ConstantInt* uniqueNess(BitVector &instBit)
    {
      int varCnt=0;
      //errs()<<"find uniqueNess!"<<"\n";
      ConstantInt* res=nullptr;
      for(uint i=0;i<instBit.size();++i)
      {
        ConstantInt* value=nullptr;
        if(instBit[i])
        {
          errs()<<*orderSsaVar[i]<<"\n";
          ++varCnt;
          auto var=dyn_cast<CallInst>(orderSsaVar[i]);
          if(var==nullptr)continue;//for storeinst and load inst
          auto varName=var->getCalledFunction()->getName();
          if(varName=="CAT_new")
          {
            value=dyn_cast<ConstantInt>(var->getOperand(0));
          }
          else if(varName=="CAT_set")
          {
            value=dyn_cast<ConstantInt>(var->getOperand(1));
            if(auto phi=dyn_cast<PHINode>(var->getOperand(1)))
            {
              if(auto con=dyn_cast<ConstantInt>(phi->getOperand(0)))
              {
                value=con;
              }
            }
          }
          else if(varName=="CAT_add"||varName=="CAT_sub")
          {
            auto op1=var->getOperand(1);
            auto op2=var->getOperand(2);
            if(isa<ConstantInt>(op1)&&isa<ConstantInt>(op2))
            {
              auto v1=dyn_cast<ConstantInt>(op1)->getSExtValue();
              auto v2=dyn_cast<ConstantInt>(op2)->getSExtValue();
              value=ConstantInt::get(IntegerType::getInt64Ty(var->getFunction()->getContext()),(varName=="CAT_add"?v1+v2:v1-v2),true);
            }
          }
        }
        if(value)
        {
          if(res==nullptr||value->getSExtValue()==res->getSExtValue())
          {
            --varCnt;
            res=value;
          }
        }
        if(varCnt>=1)return nullptr;
      }
      return res;
    }
    ConstantInt* phiOperand(Value* phi,CallInst* inst)
    {
      //errs()<<"find phi for instruction "<<*inst<<"\n";
      ConstantInt* res=nullptr;
      int varCnt=0;
      auto phiIns=dyn_cast<PHINode>(phi);
      queue<PHINode*>q;
      q.push(phiIns);
      while(!q.empty())
      {
        auto tmp=q.front();
        q.pop();
        for(uint i=0;i<tmp->getNumOperands();++i)
        {
          ConstantInt* value=nullptr;
          auto var=tmp->getOperand(i);
          if(auto varCal=dyn_cast<CallInst>(var))
          {
            if(varCal->getCalledFunction()->getName()=="CAT_new")
            {
              if(isa<ConstantInt>(varCal->getOperand(0)))
              {
                ++varCnt;
                auto phiBit=IN[inst];
                //errs()<<"the var is "<<*varCal<<"\n";
                //errs()<<"get the phi bit!"<<"\n";
                //printBit(phiBit);
                phiBit&=mustAlias[ssaVarOrder[varCal]];
                //printBit(mustAlias[ssaVarOrder[varCal]]);
                //printBit(phiBit);
                value=uniqueNess(phiBit);
                //if(value)errs()<<value->getSExtValue()<<"\n";
                //else errs()<<"nullptr"<<"\n";
              }
            }
          }
          else if(isa<PHINode>(var)){
            /*
            if(tmp->getNumOperands()==1)
            {
              tmp->replaceAllUsesWith(var);
            }
            */
            q.push(dyn_cast<PHINode>(var));
          }
          if(value)
          {
            if(res==nullptr|| value->getSExtValue()==res->getSExtValue())
            {
              --varCnt;
              res=value;
            }
          }
          if(varCnt>=1)return nullptr;
        }
      }
      errs()<<"find shit for phiNode "<<*phi<<" "<<*inst<<'\n';
      return res;
    }
    bool deadCodeCheck(CallInst* inst)
    {
      auto funcName=inst->getCalledFunction()->getName();
      //errs()<<"check dead for inst:"<<*inst<<"\n";
      if(funcName=="CAT_set")
      {
        auto var=inst->getOperand(0);
        //errs()<<"check dead code:"<<var->getNumUses()<<"\n";
        return var->getNumUses()>0;
      }
      else
      {
        //errs()<<"check dead code:"<<inst->getNumUses()<<"\n";
        return inst->getNumUses()>0;
      }
      return false;
    }
    ConstantInt* constantFolding(CallInst* inst)
    {
      auto funcName=inst->getCalledFunction()->getName();
      ConstantInt* res=nullptr;
      if(funcName=="CAT_get")
      {
        //errs()<<"do folding for inst "<<*inst<<"::\n";
        auto op=inst->getOperand(0);
        if(auto var=dyn_cast<CallInst>(op))
        {
          //errs()<<*var<<"\n";
          auto instBit=IN[inst];
          //errs()<<"its in set"<<"\n";
          //printBit(instBit);
          instBit&=mustAlias[ssaVarOrder[var]];
          //printBit(mustAlias[ssaVarOrder[var]]);
          //printBit(instBit);
          res=uniqueNess(instBit);
        }
        else if(auto var=dyn_cast<PHINode>(op))
        {
          //printBit(IN[inst]);
          res=phiOperand(var,inst);
        }
        //if(res)errs()<<"the value is"<<res->getSExtValue()<<"\n";
      }
      else if(funcName=="CAT_add"||funcName=="CAT_sub")
      {
        //errs()<<"do folding for inst "<<*inst<<"::\n";
        auto op1=inst->getOperand(1);
        auto op2=inst->getOperand(2);
        if(funcName=="CAT_sub"&&op1==op2)//special situation
        {
          return ConstantInt::get(IntegerType::getInt64Ty(inst->getFunction()->getContext()),0,true);
        }
        ConstantInt* v1=nullptr,*v2=nullptr;
        if(isa<ConstantInt>(op1))v1=dyn_cast<ConstantInt>(op1);
        else if(isa<PHINode>(op1))v1=phiOperand(op1,inst);
        else if(auto varCal=dyn_cast<CallInst>(op1))
        {
          
          //errs()<<"op1:"<<*varCal<<"\n";
          auto opBit=IN[inst];
          //errs()<<*varCal<<"\n";
          //printBit(opBit);
          mustAlias[ssaVarOrder[varCal]];
          //printBit(mustAlias[ssaVarOrder[varCal]]);
          opBit&=mustAlias[ssaVarOrder[varCal]];
          v1=uniqueNess(opBit);
          //printBit(opBit);
        }
        if(isa<ConstantInt>(op2))v2=dyn_cast<ConstantInt>(op2);
        else if(isa<PHINode>(op2))v2=phiOperand(op2,inst);
        else if(auto varCal=dyn_cast<CallInst>(op2))
        {
          //errs()<<*varCal<<"\n";
          //errs()<<"op2:"<<*varCal<<"\n";
          auto opBit=IN[inst];
          //errs()<<*varCal<<"\n";
          //printBit(opBit);
          //printBit(mustAlias[ssaVarOrder[varCal]]);
          opBit&=mustAlias[ssaVarOrder[varCal]];
          v2=uniqueNess(opBit);
          //printBit(opBit);
        }
        if(v1&&v2)
        {
          auto value1=v1->getSExtValue();
          auto value2=v2->getSExtValue();
          res=ConstantInt::get(IntegerType::getInt64Ty(inst->getFunction()->getContext()),funcName=="CAT_add"?value1+value2:value1-value2,true);
          //errs()<<"value is:"<<res->getSExtValue()<<"\n";
        }
      }
      return res;
    }
    bool aliasOptimization(Function& F)//alias modify
    {
      AliasAnalysis &aa = getAnalysis<AAResultsWrapperPass>(F).getAAResults();
      unordered_set<Instruction*>killList;
      for(auto& I:instructions(F))
      {
        if(isPause)break;
        if(auto inst=dyn_cast<CallInst>(&I))
        {
          auto op=inst->getOperand(0);
          for(auto [var,index]:ssaVarOrder)
          {
            if(auto calVar=dyn_cast<CallInst>(var))
            {
              if(calVar->getCalledFunction()->getName()=="CAT_new")
              {
                switch(aa.alias(op,var))
                {
                  case AliasResult::MustAlias:
                    killList.insert(inst);
                    break;
                  default:
                    break;
                }
              }
            }
          }
        }
      }
      for(auto& I:killList)I->eraseFromParent();
      return false;
    }
    BitVector getModify(CallInst* inst)
    {
      BitVector ans(totalCnt,false);
      //errs()<<"finding modify ptr for ins "<<*inst<<"\n";
      AliasAnalysis &aa = getAnalysis<AAResultsWrapperPass>(*(inst->getFunction())).getAAResults();
      for(uint i=0;i<inst->getNumOperands();++i)
      {
        uint size=0;
        if(auto op=dyn_cast<CallInst>(inst->getOperand(i)))
        {
          if(auto type=dyn_cast<PointerType>(inst->getType()))
          {
            auto elementPointedType=type->getNonOpaquePointerElementType();
            if(elementPointedType->isSized())
            {
              size=inst->getModule()->getDataLayout().getTypeStoreSize(elementPointedType);
            }
          }
          if(op->getCalledFunction()->getName()=="CAT_new")
          {
            switch(aa.getModRefInfo(inst,op,size))
            {
              case ModRefInfo::ModRef:
              case ModRefInfo::MustModRef:
              case ModRefInfo::Mod:
                ans|=mustAlias[ssaVarOrder[op]];
                //errs()<<"it modified "<<op<<"\n";
                break;
              default:
                break;
            }
          }
        }
      } 
      ans[ssaVarOrder[inst]]=false;
      return ans;
    }
    bool hasCatInst(Loop* L)
    {
      for(auto bbe=L->block_begin();bbe!=L->block_end();++bbe)
      {
        auto bb=*bbe;
        for(auto& I:*bb)
        {
          if(auto inst=dyn_cast<CallInst>(&I))
          {
            auto funcName=inst->getCalledFunction()->getName();
            if(catFuncName.count(funcName)||funcName=="CAT_get")
            {
              return true;
            }
          }
        }
      }
      return false;
    }
    bool unrollAndPeelLoop(
        LoopInfo &LI, 
        Loop *loop, 
        DominatorTree &DT, 
        ScalarEvolution &SE, 
        AssumptionCache &AC,
        OptimizationRemarkEmitter &ORE,
        TargetTransformInfo &TTI)
    {
      bool modified = false;
      std::vector<Loop *> subLoops = loop->getSubLoops();
      for (auto j:subLoops)
      {
          auto subloop = &*j;
          if(subloop->getLoopDepth() >10) continue;
          if (unrollAndPeelLoop(LI, subloop, DT, SE, AC, ORE, TTI))
          {
            return true;
          }       
      }
      if(loop->getLoopDepth() >10) //skip nested loops with depth >5
          return false;
      errs()<<"\n\nTrying to unroll/peel ";
      loop->print(errs());
      
      if (!(loop->isLoopSimplifyForm()  && loop->isLCSSAForm(DT)))
      {
          errs()<<"\nloop not normalized"; 
          return false; 
      }
      errs()<<"\n\nTrying to unroll loop";
      auto tripCount=SE.getSmallConstantTripCount(loop);
      auto tripMultiple=SE.getSmallConstantTripMultiple(loop);
      auto peelingCount = 1;
      UnrollLoopOptions ULO;
      ULO.Count=2;
      ULO.AllowExpensiveTripCount=true;
      ULO.Force=false;
      ULO.ForgetAllSCEV=true;
      ULO.Runtime=false;
      ULO.UnrollRemainder=false;
      LoopUnrollResult unrolled;
      if(tripMultiple > 1)
      {
          if ((tripCount >0) && (tripCount<20))
          {
            ULO.Count=tripCount;
            peelingCount = 0;
          }
          errs() << "\n   Trip count: " << tripCount << "\n";
          errs() << "\n   Trip multiple: " << tripMultiple << "\n";
          unrolled = UnrollLoop(loop, ULO,&LI, &SE, &DT, &AC, &TTI,&ORE,true);
          if (unrolled != LoopUnrollResult::Unmodified )
          {
              errs()<<"\nloop unrolling-- success";
              return true;
          }
          else
          {
              errs()<<"\nloop unrolling -- failed";
          }
      }
      if(!canPeel(loop))
      {
        BasicBlock *LatchBlock = loop->getLoopLatch();
        BranchInst *BI = dyn_cast<BranchInst>(LatchBlock->getTerminator());
        if (!BI || BI->isUnconditional()) 
        {
            SimplifyQuery SQ(*DL, BI);
            bool rotated = LoopRotation(loop, 
                &LI, &TTI, &AC, &DT, &SE, 
                nullptr, SQ, 
                false, -1, false);
            errs()<<"\nRotated loop for peeling/unrolling";
        }
      }
      if(!canPeel(loop)) return false;
      peelingCount = 1;
      errs() << "\n   PeelingCount: " << peelingCount << "\n";
      if(peelLoop( loop, peelingCount, &LI, &SE, DT, &AC, true))
      {
          errs() << "loop peeling-- success\n";
          return false ;
      }
      else
      {
          errs()<<"loop peeling-- failed\n";
      }
      return false;
    }
    void loopTransform(Function& F)
    {
      if(F.isDeclaration())return;
      if(F.getNumUses()==0&&(&F!=mainF))return;
      if(F.getInstructionCount() > 500) return;
      auto& LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
      if(LI.empty()) return;
      auto& DT = getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
      auto& SE = getAnalysis<ScalarEvolutionWrapperPass>(F).getSE();
      auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F); 
      auto &TTI = getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
      OptimizationRemarkEmitter ORE(&F);
      errs()<<"do loop transofrm for "<<F.getName()<<'\n';
      vector<Loop*>vec;
      for(auto i:LI)
      {
        auto loop=&*i;
        if(!hasCatInst(loop))continue;
        vec.push_back(loop);
      }
      errs()<<"total loop count is:"<<vec.size()<<'\n';
      for(auto loop:vec)
      {
        unrollAndPeelLoop(
          LI, loop, DT, SE, AC, ORE, TTI
        );
      }
    }
    bool runOnModule (Module &M) override {
      for(auto&F: M)cloneTest(F);
      for(auto& F:M)loopTransform(F);
      for(auto& F:M)
      {
        init(F);
        constantPropagation(F);
      }
      return false;
    }
    bool checkRecursive(Function& F)
    {
      auto fName=F.getName();
      for(auto& I:instructions(F))
      {
        if(auto inst=dyn_cast<CallInst>(&I))
        {
          auto funcName=inst->getCalledFunction()->getName();
          if(funcName==fName)return true;
        }
      }
      return false;
    }
    void cloneTest(Function& F)
    {
      unordered_set<CallInst*>rep;
      auto fName=F.getName();
      for(auto &I:instructions(F))
      {
        if(auto inst=dyn_cast<CallInst>(&I))
        {
          auto callee=inst->getCalledFunction();
          auto funcName=callee->getName();
          if(!catFuncName.count(funcName)&&funcName!="CAT_get"&&fName!=funcName&&!checkRecursive(*callee))
          {
            //errs()<<"replace function for ins "<<*inst<<"\n";
            //errs()<<"replace "<<funcName<<"\n";
            rep.insert(inst);
          }
        }
      }
      for(auto inst:rep)
      {
        InlineFunctionInfo  IFI;
        InlineFunction(*inst, IFI);
      }
    }
    void printUse(Instruction* I)
    {
      errs()<<*I<<"::\n";
      if(auto inst=dyn_cast<CallInst>(I))
      {
        for(auto usr:inst->users())
        {
          if(auto var=dyn_cast<CallInst>(usr))
          {
            errs()<<*var<<"\n";
          }
        }
      }
      errs()<<"\n";
    }
    bool getDetele()
    {
      //errs()<<"start checking\n"<<'\n';
      for(auto& I:instructions(*mainF))
      {
        if(auto inst=dyn_cast<CallInst>(&I))
        {
          auto funcName=inst->getCalledFunction()->getName();
         // errs()<<funcName<<'\n';
          if(funcName=="CAT_variables")
          {
            return false;
          }
        }
      }
      return true;
    }
    void printInAndOut(Function &F)
    {
      errs()<<"Function \""<<F.getName()<<"\" \n";
      for(auto& I:instructions(F))
      {
        errs()<<"INSTRUCTION: "<<I<<'\n';
        errs()<<"***************** IN\n{\n";
        auto in=IN[&I];
        for(uint i=0;i<in.size();++i)
        {
          if(in[i])
          {
            errs()<<" "<<*orderSsaVar[i]<<"\n";
          }
        }
        errs()<<"}\n**************************************\n***************** OUT\n{\n";
        auto out=OUT[&I];
        for(uint i=0;i<out.size();++i)
        {
          if(out[i])
          {
            errs()<<" "<<*orderSsaVar[i]<<"\n";
          }
        }
        errs()<<"}\n**************************************\n\n\n\n";
      }
    }
    void printSet()
    {
      for(auto &k:ssaVarOrder)
      {
        auto tmp=k.first;
        errs()<<*tmp<<"  "<<k.second<<"\n";
        errs()<<"IN:";
        printBit(IN[tmp]);
        errs()<<"OUT:";
        printBit(OUT[tmp]);
        errs()<<"GEN:";
        printBit(GEN[tmp]);
        errs()<<"KILL:";
        printBit(KILL[tmp]);
      }
    }
    void printMustAlias()
    {
      errs()<<"mustAlias:"<<"\n";
      for(auto& k:mustAlias)
      {
        errs()<<*orderSsaVar[k.first]<<":\n";
        printBit(k.second);
        for(uint i=0;i<k.second.size();++i)
        {
          if(k.second[i]==true&&!ssaVarDel[i])
          {
            //errs()<<*orderSsaVar[i]<<" "<<ssaVarOrder[orderSsaVar[i]]<<"\n";
            if(orderSsaVar[i]!=nullptr)errs()<<orderSsaVar[i]<<" "<<*orderSsaVar[i]<<" "<<ssaVarOrder[orderSsaVar[i]]<<"\n";
            else errs()<<ssaVarOrder[orderSsaVar[i]]<<"\n";
          }
        }
        errs()<<"\n";
      }
    }
    void printBit(BitVector tmp)
    {
      for(uint i=0;i<tmp.size();++i)
      {
        errs()<<(tmp[i]?"1":"0");
      }
      errs()<<"\n";
    }
    void printVar()
    {
      for(auto &k:ssaVarOrder)
      {
        if(k.first)errs()<<k.second<<"  "<<*k.first<<"\n";
        else errs()<<k.second<<"\n";
      }
    }
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired< CallGraphWrapperPass >();
      AU.addRequired< AAResultsWrapperPass >();
      AU.addRequired<AssumptionCacheTracker>();
      AU.addRequired<DominatorTreeWrapperPass>();
      AU.addRequired<LoopInfoWrapperPass>();
      AU.addRequired<ScalarEvolutionWrapperPass>();
      AU.addRequired<TargetTransformInfoWrapperPass>();
    }
  };
}

// Next there is code to register your pass to "opt"
char CAT::ID = 0;
static RegisterPass<CAT> X("CAT", "Homework for the CAT class");

// Next there is code to register your pass to "clang"
static CAT * _PassMaker = NULL;
static RegisterStandardPasses _RegPass1(PassManagerBuilder::EP_OptimizerLast,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
        if(!_PassMaker){ PM.add(_PassMaker = new CAT());}}); // ** for -Ox
static RegisterStandardPasses _RegPass2(PassManagerBuilder::EP_EnabledOnOptLevel0,
    [](const PassManagerBuilder&, legacy::PassManagerBase& PM) {
        if(!_PassMaker){ PM.add(_PassMaker = new CAT()); }}); // ** for -O0