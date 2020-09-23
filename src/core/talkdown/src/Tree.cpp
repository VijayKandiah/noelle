#define DEBUG_TYPE "talkdown"

#include "llvm/IR/Metadata.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "ReportDump.hpp"
#include "Tree.hpp"
#include "Annotation.hpp"
#include "AnnotationParser.hpp"

#include <graphviz/cgraph.h>

#include <cstdio>

#include <algorithm>
#include <iostream>
#include <unordered_set>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <utility>

using namespace llvm;

namespace AutoMP
{
  FunctionTree::~FunctionTree()
  {
  }

  // returns inner-most loop container for loop l
  Node *FunctionTree::findNodeForLoop( Node *start, Loop *l ) const
  {
    if ( start->getLoop() == l )
      return start;

    for ( auto &n : start->getChildren() )
    {
      Node *found = findNodeForLoop( n, l );
      if ( found )
        return found;
    }

    return nullptr;
  }

  Node *FunctionTree::findNodeForBasicBlock( Node *start, BasicBlock *bb ) const
  {
    if ( start->getBB() == bb )
      return start;

    for ( auto &n : start->getChildren() )
    {
      Node *found = findNodeForBasicBlock( n, bb );
      if ( found )
        return found;
    }

    return nullptr;
  }

  Node *FunctionTree::findNodeForInstruction(Node *start, llvm::Instruction *i) const
  {
    return findNodeForBasicBlock( start, i->getParent() );
  }

  Node *searchUpForAnnotation(Node *start, std::pair<std::string, std::string> a)
  {
    while ( start->getParent() )
    {
      for ( auto &annot : start->annotations )
      {
        // if key and value match an annotaion of the current node, return that node
        if ( !annot.getKey().compare(a.first) && !annot.getValue().compare(a.second) )
          return start;
      }

      start = start->getParent();
    }

    return nullptr;
  }

  std::vector<Node *> FunctionTree::getNodesInPreorder(Node *start) const
  {
    std::vector<Node *> retval = { start };
    if ( start->getChildren().size() == 0 )
      return retval;
    for ( auto &n : start->getChildren() )
    {
      std::vector<Node *> child_nodes = getNodesInPreorder( n );
      retval.insert( retval.end(), child_nodes.begin(), child_nodes.end() );
    }
    return retval;
  }

  std::vector<LoopContainerNode *> FunctionTree::getAllLoopContainerNodes(void) const
  {
    std::vector<LoopContainerNode *> retval;
    std::vector<Node *> all_nodes = getNodesInPreorder( root );
    for ( auto &n : all_nodes )
      if ( n->containsAnnotationWithKey( "__loop_container" ) )
        retval.push_back( (LoopContainerNode *) n );

    return retval;
  }

  std::vector<Node *> FunctionTree::getAllLoopBasicBlockNodes(void) const
  {
    std::vector<Node *> retval;
    auto all_nodes = getNodesInPreorder( root );
    for ( auto &n : all_nodes )
    {
      if ( n->containsAnnotationWithKey("__loop_bb") )
        retval.push_back(n);
    }

    return retval;
  }

  bool FunctionTree::loopContainsAnnotation(Loop *l) const
  {
    LoopContainerNode *ln = (LoopContainerNode *) findNodeForLoop(root, l);
    assert( ln != root && "Loop container is root???" );
    assert( ln && "No loop container node for loop???" );
    Instruction *i = ln->getHeaderNode()->getBB()->getFirstNonPHI();
    AnnotationSet as = parseAnnotationsForInst( i );
    if ( !as.size() )
      return false;
    return true;
    /* if ( n->getRealAnnotations().size() ) */
    /*   return true; */
    /* return false; */
  }

  // this function adds loop-container nodes to the tree that will end up being parents to all respective subloops (both
  // subloop-container nodes and basic blocks )
  // Note that this does not add any basic blocks to the tree
  // TODO Change to LoopContainerNode
  void FunctionTree::addLoopContainersToTree(LoopInfo &li)
  {
    auto loops = li.getLoopsInPreorder(); // this shit is amazing!

    for ( auto const &l : loops )
    {
      LoopContainerNode *new_node;

      // create node with root as parent
      if ( l->getParentLoop() == nullptr )
        new_node = new LoopContainerNode( root, l );

      // find the node containing parent loop and add as child to that node
      else
      {
        Node *parent = const_cast<Node *>(findNodeForLoop( root, l->getParentLoop() )); // pretty stupid but I'll change it later maybe
        assert( parent != nullptr && "Subloop doesn't have a parent loop -- something is wrong with getLoopsInPreorder()" );
        new_node = new LoopContainerNode( parent, l );
      }

      // add an internal annotation
      AnnotationSet as{{l, "__loop_container", "yes"}, {l, "__level", std::to_string(l->getLoopDepth())}};
      new_node->addAnnotations( std::move(as) );

      // add loop line number
      MDNode *loopMD = l->getLoopID();
      if ( loopMD )
      {
        auto *diloc = dyn_cast<DILocation>(loopMD->getOperand(1));
        auto dbgloc = DebugLoc(diloc);
        new_node->setDebugLoc( std::to_string(dbgloc.getLine()) );
      }

      BasicBlock *header = l->getHeader();
      assert( header && "Loop doesn't have header!" );

      nodes.push_back( new_node ); // FIXME: remove once iterator done

      // add annotations that apply to whole loop (namely ones in the header)
      /* AnnotationSet as = parseAnnotationsForInst( header->getFirstNonPHI() ); */
      /* new_node->addAnnotations( std::move(as) ); */
    }

    // annotateLoops();
  }

  void FunctionTree::annotateBasicBlocks(void)
  {
    auto loop_bb_nodes = getAllLoopBasicBlockNodes(); // returns bb nodes of preordered loops
    for ( auto &bn : loop_bb_nodes )
    {
      Instruction *i = bn->getBB()->getFirstNonPHI();
      bn->addAnnotations( parseAnnotationsForInst(i) );
    }
  }

  // This function is pretty stupid right now as FunctionTree doesn't have an iterator yet.
  // Seems like metadata is only attached to branch instruction in loop header and not the icmp instruction before...
  void FunctionTree::annotateLoops()
  {
    auto loop_nodes = getAllLoopContainerNodes(); // returns nodes in preorder

    // traverse nodes in tree
    for ( auto &node : loop_nodes )
    {
      // inherit annotations from outer loops
      if ( node->getParent() != root )
      {
        // node->addAnnotations( node->getParent()->getRealAnnotations() );
      }

      Node *header = node->getHeaderNode();
      assert( header && "Loop doesn't have header!" );

      Instruction *i = header->getBB()->getFirstNonPHI();
      auto as = parseAnnotationsForInst( i );
      if ( as.size() == 0 )
        continue;

      // need to check if parent of that loop has the same annotations. If so then they come from the outer loop

      // found annotation in loop header, propagate to all direct children
      AnnotationSet new_annots;
      for ( auto &a : as )
      {
        new_annots.emplace( node->getLoop(), a.getKey(), a.getValue() );
      }
      node->addAnnotations( std::move(as) );
    }
  }

  // creates nodes for basic blocks that belong to a loop and links to the correct loop container node
  // NOTE: doesn't add annotations to basic block node
  void FunctionTree::addBasicBlocksToLoops(LoopInfo &li)
  {

    for ( auto &bb : *associated_function )
    {
      Loop *l = li.getLoopFor( &bb );

      // if not belonging to loop, continue
      if ( !l )
        continue;

      LoopContainerNode *insert_pt = (LoopContainerNode *) const_cast<Node *>(findNodeForLoop( root, l ));
      assert( insert_pt && "No node found for loop" );

      Node *new_node = new Node( insert_pt );
      new_node->setBB( &bb );
      AnnotationSet as;
      as.emplace(nullptr, "__loop_bb", "true");
      if ( l->getHeader() == &bb )
      {
        as.emplace(nullptr, "__loop_header", "true");
        insert_pt->setHeaderNode( new_node );
      }
      new_node->addAnnotations( std::move(as) );
      nodes.push_back( new_node );
    }
  }

  // Splits a basic block between two instructions when their respective annotations differ
  // After this is done, we should supposedly just need to fetch the annotations for the first instruction in a basic block
  // and to be able to assume that they apply to the whole block
  bool FunctionTree::splitBasicBlocksByAnnotation(void)
  {
    std::vector<Instruction *> split_points; // points to split basic blocks

    // go through all basic blocks first before performing the split to keep iterators valid
    for ( auto &bb : *associated_function )
    {
      AnnotationSet prev_annots; // seen annotations
      for ( auto &i : bb )
      {
        // XXX Once we transition to intrinsics, this will have to be changed
        if ( isa<IntrinsicInst>(&i) )
        {
          continue;
        }

        auto annots = parseAnnotationsForInst( &i );

        /* sometimes the frontend doesn't attach metadata to instructions that should have metadata attached
         * (e.g. on some getelementptr instructions)
         * for now, ignore these and consider them as having the same annotations as the previous instruction
         */

        // found mismatch -- should split basic block between i-1 and i
        // XXX I don't think this is 100% correct. It will probably fail for the following code:
        // for (...) {
        //   #pragma note noelle
        //   {
        //     printf("With annotation\n");
        //   }
        //   printf("No annotation\n");
        // }
        if ( prev_annots.size() != 0 && annots.size() != 0 && annots != prev_annots )
        {
          // invalidated_bbs.insert( i.getParent() );
          errs() << "Split point found at " << *&i << "\n";
          errs() << "Previous metadata was:\n";
          for ( const auto &m : prev_annots )
            errs() << m;
          errs() << "Current metadata is:\n";
          for ( const auto &m : annots )
            errs() << m;
          split_points.push_back( &i );
        }

        prev_annots = annots;
      }
    }

    // if no split points are found, don't do anything
    if ( !split_points.size() )
      return false;

    // actually do the basic block splitting
    for ( auto &i : split_points )
    {
      BasicBlock *old = i->getParent();
      BasicBlock *new_block = SplitBlock( old, i );
    }

    return true;
  }

  // fix when the frontend doesn't attach annotation to every instructions
  // XXX doesn't work yet with nested annotations
  bool FunctionTree::fixBasicBlockAnnotations(void)
  {
    LLVMContext &ctx = associated_function->getContext(); // a function is contained within a single context
    bool modified = false;

    for ( auto &bb : *associated_function )
    {
      MDNode *md = nullptr;

      for ( auto &i : bb )
      {
        md = i.getMetadata( "note.noelle" );
        if ( md )
          break;
      }

      // if no metadata in the basic block
      if ( !md )
        continue;

      // insert it into each instruction in the basic block
      for (auto &i : bb )
      {
        MDNode *meta = i.getMetadata("note.noelle");
        if ( !meta )
        {
          i.setMetadata("note.noelle", md);
          modified |= true;
        }
      }
    }

    return modified;
  }

  // we don't care about annotations for non-loop basic blocks
  // XXX Long-term: support #pragma omp parallel region (not necessitating "for" clause)
  void FunctionTree::addNonLoopBasicBlocks(LoopInfo &li)
  {
    for ( auto &bb : *associated_function )
    {
      Loop *l = li.getLoopFor( &bb );

      // Add blocks that don't belong to any loop to the tree as direct children of root node
      if ( !l )
      {
        Node *new_node = new Node( root );
        new_node->annotations.emplace( nullptr, "__non_loop_bb", "true" );
        new_node->setBB( &bb );
        nodes.push_back( new_node ); // BAD: remove once iterator done
      }
    }
  }

  // construct a tree for each function in program order
  // steps:
  //  1. Basic blocks that don't belong to any loops don't have any annotations. They should be direct children of the root node
  //  2. Create container nodes for each outer loop, with root as a parent
  //  3. For each subloop, create a container node that a child of the parent loop
  //  4. Annotate each loop with annotations from its header basic block
  bool FunctionTree::constructTree(Function *f, LoopInfo &li)
  {
    bool modified = false;
    std::set<BasicBlock *> visited_bbs;

    associated_function = f;

    // construct root node
    // TODO(greg): with annotation of function if there is one
    this->root = new Node();
    root->annotations.emplace( nullptr, "__root", "yes" );
    nodes.push_back( root ); // BAD: remove once iterator done

    // split basic blocks based on annotation before adding them to the tree
    modified |= splitBasicBlocksByAnnotation();

    // fix the fact that the frontend misses adding annotations to some instructions
    modified |= fixBasicBlockAnnotations();

    // add all loops containsers (including subloops) to the tree
    addLoopContainersToTree( li );

    // add all basic blocks to loop nodes
    addBasicBlocksToLoops( li );

    // add all basic blocks not in a loop
    addNonLoopBasicBlocks( li );

    // supposedly adds annotations to loop container...
    annotateLoops();

    // add annotations to basic block nodes
    // annotateBasicBlocks();

    return modified;
  }

#if 0
  SESENode *FunctionTree::getInnermostNode(Instruction *inst)
  {

  }

  SESENode *FunctionTree::getParent(SESENode *node)
  {
    return node->getParent();
  }

  SESENode *FunctionTree::getFirstCommonAncestor(SESENode *n1, SESENode *n2)
  {
    SESENode *deeper;
    SESENode *shallower;

    // if same node, return one of them
    if ( n1 == n2 )
      return n1;

    // find which node is deeper in the tree
    if ( n1->getDepth() > n2->getDepth() )
    {
      deeper = n1;
      shallower = n2;
    }
    else
    {
      deeper = n2;
      shallower = n1;
    }

    // get the two search nodes to the same depth
    while ( shallower->getDepth() != deeper->getDepth() )
      deeper = deeper->getParent();

    while ( deeper != root )
    {
      if ( deeper == shallower )
        return deeper;

      deeper = deeper->getParent();
      shallower = shallower->getParent();
    }

    // if we get to root, then there was no common ancestor
    return nullptr;
  }
#endif

  const AnnotationSet &FunctionTree::getAnnotationsForInst(Instruction *) const
  {
    assert(0 && "getAnnotationsForInst(llvm::Instruction *) not implemented yet");
  }

  const AnnotationSet &FunctionTree::getAnnotationsForInst(Instruction *i, Loop *l) const
  {
    const Node *n = findNodeForLoop(root, l);
    const BasicBlock *target = i->getParent();
    for ( const auto &bbn : n->getChildren() )
      if ( bbn->getBB() == target )
        return bbn->getAnnotations();
  }

  void FunctionTree::writeDotFile( const std::string filename )
  {
    std::FILE *fp = std::fopen(filename.c_str(), "w");
    // assert( 0 && "writeDotFile() not implemented yet" );
    Agraph_t *g = agopen((char*) filename.c_str(), Agdirected, NULL);
    Agnode_t *n0 = agnode(g, "node0", 1);
    Agnode_t *n1 = agnode(g, "node1", 1);
    Agedge_t *e = agedge(g, n0, n1, "e0", 1);
    agwrite(g, fp);
    agclose(g);
  }

  // verify that the current tree is value
  bool FunctionTree::isValidTree(void) const
  {
    /*
     * Steps to perform:
     *  1. Verify all loop container nodes are in the tree
     *  2. Verify there are no duplicate basic block nodes
     */

    return false;
  }

  void FunctionTree::printNodeToInstructionMap(void) const
  {
    for ( const auto &n : nodes )
    {
      BasicBlock *bb = n->getBB();
      Instruction *first_inst = nullptr;
      if ( bb )
        first_inst = bb->getFirstNonPHI();
      else
      {
        errs() << "Node " << n->getID() << " has no basic block\n";
        continue;
      }

      if ( !first_inst )
        errs() << "Node " << n->getID() << " has no non-PHI instructions\n";
      else
        errs() << "Node " << n->getID() << " ==> BB " << bb << ":" << *first_inst << "\n";
    }
  }

  raw_ostream &operator<<(raw_ostream &os, const FunctionTree &tree)
  {
    os << "------- FunctionTree for function " << tree.associated_function->getName().str() << " --------\n\n";
    os << "Nodes to instruction map:\n";
    tree.printNodeToInstructionMap();
    Function *af = tree.getFunction();
    os << "Contains " << tree.getNodesInPreorder(tree.root).size() << " nodes\n";
    assert( af && "Function associated with a FunctionTree null" );

    // XXX For heavy debugging
    /* for ( auto &bb : *af) */
    /* { */
    /*   for ( auto &i : bb ) */
    /*   { */
    /*     const AnnotationSet as = parseAnnotationsForInst( &i ); */
    /*     const std::pair<const llvm::Instruction *, const AnnotationSet &> inst_annot = std::make_pair(&i, as); */
    /*     os << inst_annot; */
    /*   } */
    /* } */
    return tree.root->recursivePrint( os );
  }

} // namespace llvm