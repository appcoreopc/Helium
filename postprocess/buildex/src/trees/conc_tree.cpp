
#include <assert.h>
#include <fstream>
#include <iostream>

#include "utility\defines.h"
#include "trees\trees.h"
#include "analysis\x86_analysis.h"
#include "utility\print_helper.h"

using namespace std;

#define MAX_FRONTIERS		1000
#define SIZE_PER_FRONTIER	100
#define MEM_OFFSET			200
#define MEM_REGION			(MAX_FRONTIERS - MEM_OFFSET)
#define REG_REGION			MEM_OFFSET

Conc_Tree::Conc_Tree() : Tree()
{

	frontier = new frontier_t[MAX_FRONTIERS];

	for (int i = 0; i < MAX_FRONTIERS; i++){

		frontier[i].bucket = new Node *[SIZE_PER_FRONTIER];
		frontier[i].amount = 0;
	}
	
}

Conc_Tree::~Conc_Tree()
{
	delete[] frontier;
}

int Conc_Tree::generate_hash(operand_t * opnd)
{
	if (opnd->type == REG_TYPE){
		return opnd->value / MAX_SIZE_OF_REG;
	}
	else if ((opnd->type == MEM_STACK_TYPE) || (opnd->type == MEM_HEAP_TYPE)){
		int offset = (opnd->value) % (MAX_FRONTIERS - MEM_OFFSET);
		return offset + MEM_OFFSET;
	}

	return -1;  //this implies that the operand was an immediate
}

Node * Conc_Tree::search_node(operand_t * opnd)
{
	uint32_t hash = generate_hash(opnd);

	uint64_t value = opnd->value;
	uint32_t width = opnd->width;
	for (int i = 0; i < frontier[hash].amount; i++){
		//we don't need to check for types as we seperate them out in hashing
		//could further optimize this search by having a type specific search algo.
		if ((frontier[hash].bucket[i]->symbol->value == value) && (frontier[hash].bucket[i]->symbol->width == width)){
			return frontier[hash].bucket[i];
		}
	}


	return NULL;
}

void Conc_Tree::add_to_frontier(int hash, Node * node)
{
	ASSERT_MSG(((node->symbol->type != IMM_INT_TYPE) && (node->symbol->type != IMM_FLOAT_TYPE)), ("ERROR: immediate types cannot be in the frontier\n"));

	ASSERT_MSG((frontier[hash].amount < SIZE_PER_FRONTIER), ("ERROR: bucket size is full\n"));
	frontier[hash].bucket[frontier[hash].amount++] = node;

	/*if this a memory operand we should memoize it*/
	if (node->symbol->type != REG_TYPE){
		vector<uint>::iterator it;
		it = find(mem_in_frontier.begin(), mem_in_frontier.end(), hash);
		if (it == mem_in_frontier.end()){
			mem_in_frontier.push_back(hash);
		}
	}
}

Node * Conc_Tree::create_or_get_node(operand_t * opnd)
{
	Node * node = search_node(opnd);
	if (node == NULL){
		node = new Conc_Node(opnd);
	}
	return node;
}

void Conc_Tree::remove_from_frontier(operand_t * opnd)
{
	ASSERT_MSG(((opnd->type != IMM_INT_TYPE) && (opnd->type != IMM_FLOAT_TYPE)), ("ERROR: immediate types cannot be in the frontier\n"));

	uint hash = generate_hash(opnd);

	int amount = frontier[hash].amount;
	bool move = false;

	uint64 value = opnd->value;
	uint width = opnd->width;
	for (int i = 0; i < amount; i++){
		if (move){
			frontier[hash].bucket[i - 1] = frontier[hash].bucket[i];
		}
		if ((frontier[hash].bucket[i]->symbol->value == value) && (frontier[hash].bucket[i]->symbol->width == width)){  //assumes that there cannot be two Nodes with same values
			//we found the place to remove
			move = true;
		}
	}

	if (move){

		ASSERT_MSG((frontier[hash].amount > 0), ("ERROR: at least one element should have been deleted\n"));

		int amount = --frontier[hash].amount;
		/* update memoization structure for memory operands */
		if (amount == 0 && (opnd->type != REG_TYPE)){
			for (int i = 0; i < mem_in_frontier.size(); i++){
				if (mem_in_frontier[i] == hash){
					mem_in_frontier.erase(mem_in_frontier.begin() + i);
					break;
				}
			}
		}

	}
}

void Conc_Tree::get_full_overlap_nodes(std::vector<Node *> &nodes, operand_t * opnd)
{
	ASSERT_MSG(((opnd->type != IMM_INT_TYPE) && (opnd->type != IMM_FLOAT_TYPE)), ("ERROR: immediate types cannot be in the frontier\n"));

	DEBUG_PRINT(("checking for full overlap nodes...\n"), 5);

	if (opnd->type == REG_TYPE){
		uint hash = generate_hash(opnd);
		for (int i = 0; i < frontier[hash].amount; i++){

			uint start = frontier[hash].bucket[i]->symbol->value;
			uint width = frontier[hash].bucket[i]->symbol->width;

			if (((start > opnd->value) && (start + width <= opnd->value + opnd->width)) ||
				((start >= opnd->value) && (start + width < opnd->value + opnd->width))){
				DEBUG_PRINT(("reg full overlap found\n"), 5);
				nodes.push_back(frontier[hash].bucket[i]);
			}

		}
	}
	else if ((opnd->type == MEM_HEAP_TYPE) || (opnd->type == MEM_STACK_TYPE)){

		if (debug_level > 5){
			for (int i = 0; i < mem_in_frontier.size(); i++){
				printf("%d-", mem_in_frontier[i]);
			}
			printf("\n");
		}

		for (int i = 0; i < mem_in_frontier.size(); i++){
			uint index = mem_in_frontier[i];
			for (int j = 0; j < frontier[index].amount; j++){
				uint type = frontier[index].bucket[j]->symbol->type;
				if ((type == MEM_STACK_TYPE) || (type == MEM_HEAP_TYPE)){

					uint start = frontier[index].bucket[j]->symbol->value;
					uint width = frontier[index].bucket[j]->symbol->width;

					/*check whether this memory is fully contained within the current memory operand*/
					if (((start > opnd->value) && (start + width <= opnd->value + opnd->width)) ||
						((start >= opnd->value) && (start + width < opnd->value + opnd->width))){
						DEBUG_PRINT(("reg full overlap found\n"), 5);
						nodes.push_back(frontier[index].bucket[j]);
					}

				}
			}

		}
	}
}

void Conc_Tree::split_partial_overlaps(std::vector < std::pair <Node *, std::vector<Node *> > > &nodes, operand_t * opnd, uint32_t hash)
{
	for (int i = 0; i < frontier[hash].amount; i++){

		Node * split_node = frontier[hash].bucket[i];
		uint start = frontier[hash].bucket[i]->symbol->value;
		uint width = frontier[hash].bucket[i]->symbol->width;

		vector<Node *> splits;

		if (opnd->type == split_node->symbol->type){

			if (((start >= opnd->value) && (start <= opnd->value - opnd->width - 1)) /* start within */
				&& (start + width > opnd->value + opnd->width))	/*end strictly after*/
			{
				operand_t * first = new operand_t;
				operand_t * second = new operand_t;
				*first = { split_node->symbol->type, opnd->value + width - start, { start } };
				*second = { split_node->symbol->type, width - first->width, { opnd->value + width } };

				splits.push_back(create_or_get_node(first));
				splits.push_back(create_or_get_node(second));
				nodes.push_back(make_pair(split_node, splits));

				DEBUG_PRINT(("partial - %s %s\n", opnd_to_string(first), opnd_to_string(second)), 5);
			}

			else if ((start < opnd->value) /*start strictly before*/
				&& ((start + width - 1 >= opnd->value) && (start + width - 1 <= opnd->value + opnd->width - 1))) /* end within */
			{
				operand_t * first = new operand_t;
				operand_t * second = new operand_t;
				*first = { split_node->symbol->type, opnd->value - start, { start } };
				*second = { split_node->symbol->type, width - first->width, { opnd->value } };

				splits.push_back(create_or_get_node(first));
				splits.push_back(create_or_get_node(second));
				nodes.push_back(make_pair(split_node, splits));

				DEBUG_PRINT(("partial - %s %s\n", opnd_to_string(first), opnd_to_string(second)), 5);
			}

			else if ((start < opnd->value) && (start + width > opnd->value + opnd->width)) /* strictly within start and end */ {

				operand_t * first = new operand_t;
				operand_t * second = new operand_t;

				*first = { split_node->symbol->type, opnd->value - start, { start } };
				*second = { split_node->symbol->type, width - first->width - opnd->width, { opnd->value + width } };

				splits.push_back(create_or_get_node(first));
				splits.push_back(create_or_get_node(opnd));
				splits.push_back(create_or_get_node(second));
				nodes.push_back(make_pair(split_node, splits));

				DEBUG_PRINT(("partial - %s %s %s\n", opnd_to_string(first), opnd_to_string(opnd), opnd_to_string(second)), 5);

			}
		}
	}
}

void Conc_Tree::get_partial_overlap_nodes(std::vector< std::pair<Node *, std::vector<Node *> > > &nodes, operand_t * opnd)
{
	DEBUG_PRINT(("checking for partial overlap nodes...\n"), 5);
	if (opnd->type == REG_TYPE){
		uint hash = generate_hash(opnd);
		split_partial_overlaps(nodes, opnd, hash);

	}
	else if ((opnd->type == MEM_STACK_TYPE) || (opnd->type == MEM_HEAP_TYPE)){
		for (int i = 0; i < mem_in_frontier.size(); i++){
			uint index = mem_in_frontier[i];
			split_partial_overlaps(nodes, opnd, index);
		}
	}
}

/*helper function for adding dependancies  - could have used the node transformations?*/
void add_dependancy(Node * dst, Node * src, uint operation){

	int src_index = dst->srcs.size();

	dst->srcs.push_back(src);
	if (dst->operation == -1) dst->operation = operation;

	src->prev.push_back(dst);
	src->pos.push_back(src_index);
}

bool Conc_Tree::update_depandancy_backward(rinstr_t * instr, uint32_t pc, std::string disasm, uint32_t line)
{
	//TODO: have precomputed nodes for immediate integers -> can we do it for floats as well? -> just need to point to them in future (space optimization)
	Node * head = get_head();
	
	if (head == NULL){
		head = new Conc_Node(&instr->dst);
		set_head(head);
		//head->operation = op_assign;
		int hash = generate_hash(&instr->dst);

		//we cannot have a -1 here! - give out an error in future
		if (hash != -1){
			int amount = frontier[hash].amount;
			frontier[hash].bucket[amount] = head;
			frontier[hash].amount++;
		}
	}


	vector<Node *> full_overlap_nodes;
	vector<pair<Node *, vector<Node *> > >  partial_overlap_nodes;

	/*first get the partial overlap nodes - if dest is part of the frontier of a wide region it will be part of the nodes returned*/
	get_partial_overlap_nodes(partial_overlap_nodes, &instr->dst);

	if (partial_overlap_nodes.size() > 0){
		for (int i = 0; i < partial_overlap_nodes.size(); i++){
			Node * node = partial_overlap_nodes[i].first;
			vector<Node *> overlaps = partial_overlap_nodes[i].second;
			remove_from_frontier(node->symbol);
			for (int j = 1; j < overlaps.size(); j++){
				add_dependancy(node, overlaps[j], op_partial_overlap);
				add_to_frontier(generate_hash(overlaps[j]->symbol), overlaps[j]);
			}
		}
	}

	/*get the destination -> the partial overlap may have created the destination if it was contained with in a wide mem region*/
	int hash_dst = generate_hash(&instr->dst);
	DEBUG_PRINT(("dst_hash : %d, frontier amount : %d\n", hash_dst, frontier[hash_dst].amount), 4);
	Node * dst = search_node(&instr->dst);

	/* now get the full overlap nodes */
	/* nodes that contain with in current node - we can delete these by replacing with the current destination node */
	get_full_overlap_nodes(full_overlap_nodes, &instr->dst);

	/*do we have nodes that are contain within the current dest?*/
	if (full_overlap_nodes.size() > 0){
		if (dst == NULL){
			dst = new Conc_Node(&instr->dst);
		}
		for (int i = 0; i < full_overlap_nodes.size(); i++){
			DEBUG_PRINT(("full overlap - %s\n", opnd_to_string(full_overlap_nodes[i]->symbol).c_str()), 4);
			add_dependancy(full_overlap_nodes[i], dst, op_full_overlap);
			full_overlap_nodes[i]->pc = pc;
			full_overlap_nodes[i]->line = line;
			remove_from_frontier(full_overlap_nodes[i]->symbol);
		}
	}

	if (dst == NULL){
		DEBUG_PRINT(("not affecting the frontier\n"), 4);
		return false;  //this instruction does not affect the slice
	}
	else{

		/*if (dst != head && dst->symbol->type == MEM_HEAP_TYPE){
		DEBUG_PRINT(("stopping at a heap location (intermediate node)"), 5);
		return;
		}*/

		DEBUG_PRINT(("dst - %s : affecting the frontier\n", opnd_to_string(dst->symbol).c_str()), 4);
		if (debug_level >= 6){
			DEBUG_PRINT(("current expression:\n"), 5);
			print_tree(cout);
			cout << endl;
		}

		/* we need to update the line and pc information */
		dst->line = line;
		dst->pc = pc;

	}

	//update operation
	dst->operation = instr->operation;
	DEBUG_PRINT(("operation : %s\n", operation_to_string(dst->operation).c_str()), 4);


	//ok now to remove the destination from the frontiers
	remove_from_frontier(&instr->dst);
	/* assign operation optimization - space */
	bool assign_opt = false;
	//update srcs
	for (int i = 0; i<instr->num_srcs; i++){

		//first check whether there are existing nodes in the frontier for these sources
		//if there is we know that the same definition of src is used for this, so we can just point to it rather than 
		//creating a new node -> space and time efficient
		int hash_src = generate_hash(&instr->srcs[i]);

		DEBUG_PRINT(("src_hash : %d, frontier amount : %d\n", hash_src, frontier[hash_src].amount), 4);

		bool add_node = false;
		Node * src;  //now the node can be imme or another 
		if (hash_src == -1){
			src = new Conc_Node(&instr->srcs[i]);
		}
		else{
			src = search_node(&instr->srcs[i]);
		}

		//when do we need another node? if node is not present or if the destination matches src ( e.g : i <- i + 1 )
		//we do not need to check for immediates here as src will point to a brand new Node in that case and hence will not enter 
		//the if statement
		if ((src == NULL) || (src == dst)){  //I think now we can remove the src == dst check -> keeping for sanity purposes (why? because we remove the dst from the frontier)
			src = new Conc_Node(&instr->srcs[i]);
			add_node = true;
			DEBUG_PRINT(("new node added to the frontier\n"), 4);
		}

		DEBUG_PRINT(("src - %s\n", opnd_to_string(src->symbol).c_str()), 4);

		if ((instr->num_srcs == 1) && (instr->operation == op_assign)){  //this is just an assign then remove the current node and place the new src node -> compiler didn't optimize for this?

			uint num_references = dst->prev.size();

			for (int j = 0; j < num_references; j++){

				//cout << dst << endl;
				//cout << dst->prev[i] << " " << dst->pos[i] << " " << dst->prev[i]->srcs[dst->pos[i]] << endl;

				src->prev.push_back(dst->prev[j]);
				src->pos.push_back(dst->pos[j]);
				dst->prev[j]->srcs[dst->pos[j]] = src;
				src->line = line;
				src->pc = pc;

				assign_opt = true;  //this is here to differentitate between the head and the rest of the nodes
			}


			if (instr->is_floating){
				src->is_double = instr->is_floating;
			}


			DEBUG_PRINT(("optimizing assign\n"), 4);

			if (assign_opt)  delete dst;  //we have broken all linkages, so just delete it 


		}

		/* update the tree + backward references */

		if (!assign_opt){

			uint src_index = dst->srcs.size();
			dst->srcs.push_back(src);
			src->prev.push_back(dst);
			src->pos.push_back(src_index);

			if (instr->is_floating){
				src->is_double = instr->is_floating;
			}


		}



		if (debug_level >= 6){
			DEBUG_PRINT(("current expression after adding src\n"), 5);
			print_tree(cout);
			cout << endl;
		}

		/* update the frontiers - include the sources to the frontier if new nodes created */
		if (add_node) add_to_frontier(hash_src, src);

		DEBUG_PRINT(("completed adding a src\n"), 4);

	}

	if (!assign_opt){
		//simplify_identity_add(dst);
		//simplify_identity_mul(dst);

		/* do we need to do this? can't we do it outside */
		dst->congregate_node(this->get_head());
		
	}

	return true;

}

bool Conc_Tree::update_dependancy_forward(rinstr_t * instr, uint32_t pc, std::string disasm, uint32_t line)
{
	for (int i = 0; i < instr->num_srcs; i++){

		/* check if this source is there, full o/l'ed, partial o/l'ed */

		if (instr->srcs[i].type == IMM_FLOAT_TYPE || instr->srcs[i].type == IMM_INT_TYPE) continue;


		Node * src_node = search_node(&instr->srcs[i]);


		vector<Node *> full_overlaps;
		vector<pair<Node *, vector<Node *> > > partial_overlaps;

		get_full_overlap_nodes(full_overlaps, &instr->srcs[i]);
		get_partial_overlap_nodes(partial_overlaps, &instr->srcs[i]);



		if ((src_node != NULL) || (full_overlaps.size() > 0) || (partial_overlaps.size() > 0)){
			Node * dst_node = search_node(&instr->dst);
			if (dst_node == NULL){
				add_to_frontier(generate_hash(&instr->dst), new Conc_Node(&instr->dst));
			}
			return true;
		}

	}

	/*if no src is found, we should remove the dst, if it is already there*/
	Node * dst_node = search_node(&instr->dst);
	if (dst_node != NULL) remove_from_frontier(&instr->dst);


	return false;
}

bool Conc_Tree::update_dependancy_forward_with_indirection(rinstr_t * instr, uint32_t pc, std::string disasm, uint32_t line){


	for (int i = 0; i < instr->num_srcs; i++){

		/* check if this source is there, full o/l'ed, partial o/l'ed */
		vector<operand_t *> srcs;

		srcs.push_back(&instr->srcs[i]);
		if (instr->srcs[i].addr != NULL){
			for (int j = 0; j < 4; j++){
				srcs.push_back(&instr->srcs[i].addr[j]);
			}
		}
		if (instr->dst.addr != NULL){
			for (int j = 0; j < 4; j++){
				srcs.push_back(&instr->dst.addr[j]);
			}
		}


		for (int j = 0; j < srcs.size(); j++){

			if (srcs[j]->type == IMM_FLOAT_TYPE || srcs[j]->type == IMM_INT_TYPE) continue;
			if (srcs[j]->type == REG_TYPE && srcs[j]->value == 0) continue; //NULL registers

			Node * src_node = search_node(srcs[j]);


			vector<Node *> full_overlaps;
			vector<pair<Node *, vector<Node *> > > partial_overlaps;

			get_full_overlap_nodes(full_overlaps, srcs[j]);
			get_partial_overlap_nodes(partial_overlaps, srcs[j]);



			if ((src_node != NULL) || (full_overlaps.size() > 0) || (partial_overlaps.size() > 0)){
				Node * dst_node = search_node(&instr->dst);
				if (dst_node == NULL){
					add_to_frontier(generate_hash(&instr->dst), new Conc_Node(&instr->dst));
				}
				return true;
			}
		}

	}

	/*if no src is found, we should remove the dst, if it is already there*/


	Node * dst_node = search_node(&instr->dst);
	if (dst_node != NULL) remove_from_frontier(&instr->dst);

	return false;


}


void Conc_Tree::print_conditionals(){
	
	cout << "printing conditionals" << endl;
	cout << conditionals.size() << endl;
	for (int i = 0; i < conditionals.size(); i++){
		cout << conditionals[i]->jump_info->cond_pc << " " << conditionals[i]->line_cond << endl;
	}

}


/* to be implemented */



std::string Conc_Tree::serialize_tree()
{
	throw "not implemented!";
}

void Conc_Tree::construct_tree(std::string stree)
{
	throw "not implemented!";
}



