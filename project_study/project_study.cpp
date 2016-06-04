﻿#include <boost/config.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/utility.hpp>
#include <boost/property_map/property_map.hpp>
#include <boost/graph/filtered_graph.hpp>
#include <iostream>
#include <list>
#include <string>
#include <fstream>
#include <sstream>
#include <utility>
#include <map>
#include <tuple>
#include <ctime>
#include <vector>
#include <iterator>
#include <chrono>
#include "yen_ksp.hpp"

#define G 1
#define M_MAX 4
#define B 300
#define Cslot 12.5
#define GB 7

//全域變數 
//決定使用何種權重
enum Priority {d_prime, max_block, fr};
Priority g_expand_priority = Priority::max_block;

enum R_Priority { pathweight_slot, fragmentation_rate, slot_big_first, slot_small_first };
R_Priority g_reduce_priority = R_Priority::pathweight_slot;

enum CUT_Priority { cut_from_left, cut_from_right };
CUT_Priority g_reduce_cut_priority = CUT_Priority::cut_from_right;


//輸出結果的stringstream
std::stringstream result_ss;
//std::ofstream result_ss("result.txt", std::ios_base::trunc);

#include "debug.hpp"
#include "add.hpp"
#include "expand.hpp"
#include "reduce_algo.hpp"
#include "delete_algo.hpp"
#include "fr.hpp"

using namespace boost;


//vertex & edge properties 別名
//巢狀 property typedef 下面為 property 模板的原型, 第三個參數預設為 no_property
//property<class PropertyTag, class T, class NextProperty = no_property>
using VertexProperties = property<vertex_name_t, std::string>;
using EdgeProperties = property < edge_weight_t, int, property<edge_capacity_t, int,
	property<edge_index_t, int, property<edge_weight2_t, double> > > >;

//graph 別名
//對於 EdgeList 使用 hash_setS 強制 graph 不會出現平行邊
//vecS
//undirectedS  
//VertexProperties 頂點的屬性(頂點名稱,型別string)
//EdgeProperties 邊的屬性(權重,型別int)(容量,型別int)(邊號碼,型別int)(權重2,型別int)
using Graph = adjacency_list< hash_setS, vecS, undirectedS, VertexProperties, EdgeProperties >;

//vertex & edge descriptor 別名
//可以把 descriptor 當成獨特的 ID, 每個 vertex 或 edge 都有屬於自己的 descriptor(ID)
using Vertex = graph_traits<Graph>::vertex_descriptor;		//以 Vertex 做為 vertex_descriptor 的別名
using Edge = graph_traits<Graph>::edge_descriptor;			//以 Edge 做為 edge_descriptor 的別名

//property map type 別名
//用來表示 property map 物件的型別
using VertexNameMap = property_map<Graph, vertex_name_t>::type;
using EdgeWeightMap = property_map<Graph, edge_weight_t>::type;
using EdgeCapacityMap = property_map<Graph, edge_capacity_t>::type;
using EdgeIndexMap = property_map<Graph, edge_index_t>::type;

//std::map 別名
//宣告 map 物件作為檔案讀入時的安插用 map (並非 BGL 的 property map), 為全域變數
//property map 為(Vertex, name), 對於 name 當 key 的尋找不方便
//故另行使用(name, Vertex)的 map 方便以 name 當 key 的尋找
using MapOfName = std::map<std::string, Vertex>;
MapOfName g_vertexNameMap;





//使用 map 來記錄對於同個 src & dst 的 Request 的路徑
struct UsingPathDetail
{
	std::list<Graph::edge_descriptor> edge_list;
	int slot_begin;
	int slot_num;

	bool operator== (const UsingPathDetail& rhs) const
	{
		return (this->edge_list == rhs.edge_list) && (this->slot_begin == rhs.slot_begin)
			&& (this->slot_num == rhs.slot_num);
	}

};

//UsingPathDetail 的比較原則
struct UsingPathCmp
{
	bool operator() (const UsingPathDetail& lhs, const UsingPathDetail& rhs) const
	{
		return lhs.edge_list < rhs.edge_list;
	}

};

std::map<std::pair<Vertex, Vertex>, std::multiset<UsingPathDetail, UsingPathCmp> > g_usingPaths;



//自定義 strcut, 用來表示網路需求
struct Request
{
	Vertex src;		//soucre
	Vertex dst;		//destination
	double cap;		//capacity
};

//從 filestream 建立 graph, 參數都以參照傳入
template<typename Graph>
void construct_graph(std::ifstream& file_in, Graph& g)
{
	//property map 宣告, 以 get(property_tag, graph) 函式取得 property map 物件
	VertexNameMap name_map = get(vertex_name, g);
	EdgeWeightMap weight_map = get(edge_weight, g);
	EdgeCapacityMap capacity_map = get(edge_capacity, g);
	EdgeIndexMap edge_index_map = get(edge_index, g);

	//以 property_traits 取得 property map 裡存放 value 所對應的 type
	//(key, →value←)
	//其實就是property<class PropertyTag, class T> 裡的class T
	//這邊並沒有另行使用別名, 直接宣告變數
	//typename 代表後面 →property_traits<>::value_type← 為 type
	typename property_traits<VertexNameMap>::value_type s, t;
	typename property_traits<EdgeWeightMap>::value_type weight;
	typename property_traits<EdgeCapacityMap>::value_type capacity;
	typename property_traits<EdgeIndexMap>::value_type edge_index;

	edge_index = 0;

	//用 getline() 一次讀一行
	for (std::string line; std::getline(file_in, line);)
	{
		std::istringstream(line) >> s >> t >> capacity >> weight;


		//insert 函式回傳 map 的安插結果, 型別為一個pair 
		//first 元素為新安插的位置 or 先前已安插過的位置
		//second 元素為是否有安插成功
		//安插成功的話則進行 add_vertex(), name_map 的安插
		//否則將 vertex descriptor 設定為已經在 map 裡的 vertex
		MapOfName::iterator pos;//map 物件的 iterator, 對其提領得到一組 map, (name, vertex_descriptor)
		bool inserted;
		Vertex u, v;//u,v=vertex_descriptor
		tie(pos, inserted) = g_vertexNameMap.insert(std::make_pair(s, Vertex()));
		if (inserted)//安插成功
		{
			u = add_vertex(g);//add_vertex()回傳新增到 graph 裡的 vertex 的 vertex_descriptor
			name_map[u] = s;//(property map) name_map[vertex_descriptor]=s, s 為頂點名稱
			pos->second = u;//(非 property map) g_vertexNameMap[name] = u, u 為 vertex_descriptor 
		}
		else
			u = pos->second;

		tie(pos, inserted) = g_vertexNameMap.insert(std::make_pair(t, Vertex()));
		if (inserted)
		{
			v = add_vertex(g);
			name_map[v] = t;
			pos->second = v;
		}
		else
			v = pos->second;

		//add edge
		//若對 edge 安插失敗則代表 graph_input.txt 中有重複的 edge, 顯示錯誤訊息
		Edge e;
		tie(e, inserted) = add_edge(u, v, g); //add_edge 回傳 (edge_descriptor, inserted)
		if (inserted)
		{
			weight_map[e] = weight;
			capacity_map[e] = capacity;
			edge_index_map[e] = edge_index++;
		}
		else
		{
			std::cerr << "Detect repeat edge in graph_input.txt file!!" << std::endl << std::endl;
		}


	}


}


int main()
{
	std::ifstream file_in("graph_input.txt");


	//如果沒 graph_input.txt, 顯示錯誤訊息並離開程式
	if (!file_in)
	{
		std::cerr << "No graph_input.txt file!!" << std::endl;
		return EXIT_FAILURE;
	}

	Graph graph;

	//property map
	//以 get(property_tag, graph) 函式取得 property map 物件
	VertexNameMap name_map = get(vertex_name, graph);
	EdgeWeightMap weight_map = get(edge_weight, graph);
	EdgeCapacityMap capacity_map = get(edge_capacity, graph);


	//construct the graph把圖建立
	construct_graph(file_in, graph);

	Request request;//(src,dst,cap)

	//測試add & expand 使用request1.txt//////////////////
	//std::ifstream file_request("request1.txt");

	//測試expand 使用request2.txt/////////////////////////
	//std::ifstream file_request("request2.txt");

	//測試 add expand delete reduce 使用 resquest_test.txt
	std::ifstream file_request("request_test.txt");


	if (!file_request)
	{
		std::cerr << "No request1.txt file!!" << std::endl;
		return EXIT_FAILURE;
	}

	//exterior_properties test
	//用 2 維 vector 儲存每條 edge 的 bit_mask, 以 exterior_properties 的方式儲存
	std::vector<std::vector<int>> edge_bit_mask(num_edges(graph), std::vector<int>(B, 0));
	EdgeIndexMap edge_index_map = get(edge_index, graph);
	using IterType = std::vector<std::vector<int>>::iterator;
	using IterMap = iterator_property_map<IterType, EdgeIndexMap>;
	IterType bit_mask_iter = edge_bit_mask.begin();

	/////輸出分配結果測試////
	int req_num = 1;
	//設定ostringstream buffer大小為10K
	result_ss.rdbuf()->pubsetbuf(NULL, 10240);
	/////////////////////////

	//------------------------收集實驗結果---------------------------//
	int fail_times = 0;
	int add_times=0,expand_times=0,reduce_times=0,delete_times=0;
	//每10筆紀錄四種方法
	int ten_add=0,ten_expand=0,ten_reduce=0,ten_delete=0;
    //計算阻塞的次數
	double block_times=0;
	//計算阻塞率的部分
	double block_rate=0;
	int real_total =0;
	int real_total_old =0;

	//碎片率紀錄的檔案串流
	std::stringstream fr_result_ss;
	//統計4種方法的檔案串流
	std::stringstream statistic_ss;
	//阻塞率的檔案串流
	std::stringstream block_rate_ss;
	//使用連線總數的檔案串流
	std::stringstream used_path_ss;
	//計算cpu run time
	std::stringstream run_time_ss;
	//計時
	auto timer_1 = std::chrono::high_resolution_clock::now();
	auto initial_time = std::chrono::high_resolution_clock::now();
    //---------------------------------------------------------------//



	std::cout << "running..." << std::endl;
	
	for (std::string line; std::getline(file_request, line);)
	{
		/////輸出分配結果測試////
		result_ss << "request " << req_num << "  ";
		/////////////////////////////

		std::string src_name, dst_name;
		std::istringstream(line) >> src_name >> dst_name >> request.cap;

		//find(key) 回傳所尋找到的第一組 (key, vaule) 的位置 (也是iterator)
		//在此使用 find 而不使用更簡單的 map[key], 確保如果所搜尋的 key 若不在 map 中會使程式報錯
		auto src_pos = g_vertexNameMap.find(src_name);
		auto dst_pos = g_vertexNameMap.find(dst_name);
		request.src = src_pos->second;
		request.dst = dst_pos->second;


		//在此判斷需求類型
		std::string req_type;
		std::pair<Vertex, Vertex> vertex_pair = std::make_pair(request.src, request.dst);
		auto find_result = g_usingPaths.find(vertex_pair);

		//(src, dst) 在 g_usingPath 未出現過 代表新增
		if (find_result == g_usingPaths.end() && request.cap > 0)
			req_type = "add";
		else if (request.cap == 0)
			req_type = "delete";
		else if (find_result != g_usingPaths.end() && request.cap > 0)
			req_type = "expand";
		else if (request.cap < 0)
			req_type = "reduce";

		/////輸出分配結果測試////
		result_ss << "mode: " << req_type << std::endl;
		result_ss << "request detail: " << "s=" << src_name << ", d=" << dst_name << ", C=" << request.cap;
		result_ss << std::endl << std::endl;
		////////////////////////
        
		//開始針對需求進行分配
		bool success = false;
		bool failed = false;

		if (req_type == "add")
		{
			success = add(graph, request, IterMap(bit_mask_iter, edge_index_map));
			
			//----------收集實驗結果--------------//
			add_times++;
			ten_add++;
			if(!success)
				block_times++; 
			//--------------------------------------//
		}
		else if (req_type == "delete")
		{
			success = delete_algo(graph, g_usingPaths, request, IterMap(bit_mask_iter, edge_index_map));
			//----------收集實驗結果--------------//
			if (!success)
			{
				--real_total;	
				failed = true;
			}
			else
			{
				delete_times++;
			    ten_delete++;				
			}
			//----------------------------------//
		}
		else if (req_type == "expand")
		{
			success = expand(graph, request, IterMap(bit_mask_iter, edge_index_map));
			if (!success)//擴充失敗時改用新增
			{
				success = add(graph, request, IterMap(bit_mask_iter, edge_index_map));
			//----------收集實驗結果--------------//
				add_times++;
				ten_add++;
				if(!success)
					block_times++; 		
			}


			expand_times++;
			ten_expand++;
			//---------------------------------//
			
		}
		else if (req_type == "reduce")
		{
			success = reduce_algo(graph, g_usingPaths, request, IterMap(bit_mask_iter, edge_index_map));
			//----------收集實驗結果--------------//
			if (!success)
			{
				--real_total;
				failed = true;
			}
			else
			{
				reduce_times++;
			    ten_reduce++;				
			}
			//------------------------------------//
		}
		//------------------------------------收集實驗結果-----------------------------------------//
		
		auto timer_account_start = std::chrono::high_resolution_clock::now();
		

		//計算阻塞率的部分
		++real_total;
		if (real_total % 10 == 0 && real_total != real_total_old)
		{						
			if (block_times <= 0)
				block_times = 0;

			block_rate = (double)block_times / real_total;
			block_rate_ss << block_rate << std::endl;

		}
		real_total_old = real_total;

		if (req_num % 10 == 0)
		{
			//計時
			auto timer_2 = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double> during_time = timer_2 - timer_1;
			run_time_ss << during_time.count() << std::endl;
			timer_1 = std::chrono::high_resolution_clock::now();
		
			//計算add/expand/reduce/delete部分
			//累計的部分
			statistic_ss << "After " << req_num << " requests, add times: " << add_times << std::endl;
			statistic_ss << "After " << req_num << " requests, expand times: " << expand_times << std::endl;
			statistic_ss << "After " << req_num << " requests, reduce times: " << reduce_times << std::endl;
			statistic_ss << "After " << req_num << " requests, delete times: " << delete_times << std::endl;
			//每10筆的部分
			statistic_ss << "In " << req_num - 10 << "~" << req_num << " times add number: " << ten_add << std::endl;
			statistic_ss << "In " << req_num - 10 << "~" << req_num << " times expand number: " << ten_expand << std::endl;
			statistic_ss << "In " << req_num - 10 << "~" << req_num << " times reduce number: " << ten_reduce << std::endl;
			statistic_ss << "In " << req_num - 10 << "~" << req_num << " times delete number: " << ten_delete << std::endl;
			ten_add = 0; ten_expand = 0; ten_reduce = 0; ten_delete = 0;


			//計算正在使用的連線總數
			int used_link_num = 0;
			for (const auto& usingPath : g_usingPaths)
				used_link_num += usingPath.second.size();
			used_path_ss << used_link_num << std::endl;


			//計算碎片率部份
			Graph::edge_iterator edge_iter, edge_iter_end;
			tie(edge_iter, edge_iter_end) = edges(graph);
			double fr_sum = 0;
			for (; edge_iter != edge_iter_end; ++edge_iter)
				fr_sum += edge_fr(*edge_iter, IterMap(bit_mask_iter, edge_index_map));			
			fr_result_ss << fr_sum / num_edges(graph) << std::endl;
			
		}
		

		/////輸出分配結果測試////
		result_ss << std::endl << "result: ";
		if (success)
			result_ss << "success!" << std::endl;
		else if (!success && !failed)
			result_ss << "block!" << std::endl;
		else if (!success && failed)
			result_ss << "undefined! do nothing!" << std::endl;
		result_ss << std::endl << "------------------------------------" << std::endl;
		////////////////////////

		//debug//測試//////////
		//bit_mask_print_separate(graph, edge_bit_mask, IterMap(bit_mask_iter, edge_index_map));
		++req_num;
		

		//扣除收集實驗結果和輸出分配的時間
		auto timer_account_end = std::chrono::high_resolution_clock::now();
		timer_1 += timer_account_end - timer_account_start;
		initial_time += timer_account_end - timer_account_start;
		//---------------------------------------------------------------------//
		
	}




	auto final_time = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> working_time = final_time - initial_time;
	run_time_ss << working_time.count() << std::endl;
	
	//cpu run time 檔案輸出
	std::ofstream file_run_time("result_data/runtime.txt", std::ios_base::trunc);
	file_run_time << run_time_ss.rdbuf();
	file_run_time.close();
	
	/////輸出分配結果測試////
	std::ofstream file_result("result_data/result.txt", std::ios_base::trunc);
	file_result.rdbuf()->pubsetbuf(NULL, 10240);
	file_result << result_ss.rdbuf();
	file_result.close();
	//////////////////////////

    //碎片率檔案輸出
	std::ofstream file_fr_result("result_data/fr_result.txt", std::ios_base::trunc);
	file_fr_result << fr_result_ss.rdbuf();
	file_fr_result.close();

	//統計數據檔案輸出
	std::ofstream file_static("result_data/static.txt", std::ios_base::trunc);
	file_static << statistic_ss.rdbuf();
	file_static.close();
	
	//阻塞率檔案輸出 block_rate_ss
	std::ofstream file_block_rate("result_data/blockrate.txt", std::ios_base::trunc);
	file_block_rate << block_rate_ss.rdbuf();
	file_block_rate.close();
	
	//使用的連線數量的檔案串流 used_path_ss
	std::ofstream file_used_path("result_data/UsingPathNumber.txt", std::ios_base::trunc);
	file_used_path << used_path_ss.rdbuf();
	file_used_path.close();
	
	//測試/////////////////
	//印出usingPaths
	/*print_usingPaths(graph, g_usingPaths);*/
	//////////////////////

	return 0;
}

