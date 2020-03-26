#include "vm.h"

#include <algorithm>
#include <iostream>
#include <thread>

using namespace std;

VM::VM(virConnectPtr &connPtr, string &name) {
	if (connPtr == NULL) throw invalid_argument("Invalid connection object\n"s);
	vector<string> names = getInactiveDomainNames(connPtr);
	if (find(names.begin(), names.end(), name) == names.end()) {
		throw invalid_argument(
			"VM::VM(): no inactive VM found with name=" + name + "\n");
	}
	domPtr = virDomainLookupByName(connPtr, name.c_str());
	if (domPtr == NULL) {
		throw failed_call_exception("VM::VM()", "virDomainLookupByName");
	}
}

VM::VM(virConnectPtr &conn) {
	vector<string> inactiveDomains = getInactiveDomainNames(conn);
	virDomainPtr dom;
	if (inactiveDomains.empty()) {
		throw runtime_error(
			"VM::startAnyInactiveDomain: no inactive domain configuration "
			"found");
	} else {
		cout << "VM::startAnyInactiveDomain: starting domain with name "
			 << inactiveDomains.at(0) << endl;
		dom = virDomainLookupByName(conn, inactiveDomains.at(0).c_str());
		if (virDomainCreate(dom) < 0) {
			throw runtime_error(
				("VM::startAnyInactiveDomain: Unable to boot guest "
				 "configuration for" +
				 inactiveDomains.at(0)));
		}
	}
	domPtr = dom;
}

VM::~VM() { virDomainFree(domPtr); }

string VM::GetTypedParamValue(virTypedParameterPtr item) {
	string str;
	if (item == NULL) {
		cerr << "VM::GetTypedParamValue : NULL item passed" << endl;
		return str;
	}

	switch (item->type) {
		case VIR_TYPED_PARAM_INT:
			str = to_string(item->value.i);
			break;

		case VIR_TYPED_PARAM_UINT:
			str = to_string(item->value.ui);
			break;

		case VIR_TYPED_PARAM_LLONG:
			str = to_string(item->value.l);
			break;

		case VIR_TYPED_PARAM_ULLONG:
			str = to_string(item->value.ul);
			break;

		case VIR_TYPED_PARAM_DOUBLE:
			str = to_string(item->value.d);
			break;

		case VIR_TYPED_PARAM_BOOLEAN:
			if (item->value.b)
				str = "yes";
			else
				str = "no";
			break;

		case VIR_TYPED_PARAM_STRING:
			str = string(item->value.s);
			break;

		default:
			cerr << "unimplemented parameter type " << item->type << endl;
	}
	return str;
}

unordered_map<string, string> VM::getDomainStatRecord(
	virDomainStatsRecordPtr record) {
	unordered_map<string, string> map;
	string param;
	if (record == nullptr or record == NULL) {
		cerr << "VM::getDomainStatRecord: NULL record passed" << endl;
		return map;
	}
	map["domain.name"] = string(virDomainGetName(record->dom));
	for (int i = 0; i < record->nparams; i++) {
		param = GetTypedParamValue(record->params + i);
		if (!param.empty()) map[string(record->params[i].field)] = param;
	}
	return map;
}

vector<string> VM::getInactiveDomainNames(virConnectPtr &conn) {
	vector<string> names;
	int numDomains = virConnectNumOfDefinedDomains(conn);
	if (numDomains == -1) {
		return names;
	}
	names.reserve(numDomains);
	char *inactiveDomainsNames[numDomains];
	numDomains =
		virConnectListDefinedDomains(conn, inactiveDomainsNames, numDomains);
	for (int i = 0; i < numDomains; i++) {
		names.emplace_back(inactiveDomainsNames[i]);
	}
	return names;
}

unordered_map<string, string> VM::getStatsforDomain(virConnectPtr &conn) {
	int statFlag = VIR_DOMAIN_STATS_VCPU | VIR_DOMAIN_STATS_CPU_TOTAL |
				   VIR_DOMAIN_STATS_STATE;

	unordered_map<string, string> currMap, testMap;
	int status = 0;
	virDomainStatsRecordPtr *records = NULL;
	virDomainStatsRecordPtr *next = NULL;

	status = virConnectGetAllDomainStats(conn, statFlag, &records, 0);
	if (status == -1) {
		cerr << "VM::getStatsforDomain: call to "
				"virConnectGetAllDomainStats failed"
			 << endl;
	} else if (records == NULL) {
		cerr << "VM::getStatsforDomain: no stat data returned from "
				"virConnectGetAllDomainStats"
			 << endl;
	} else {
		next = records;
		while (*next) {
			testMap = getDomainStatRecord(*next);
			if (testMap.at("domain.name") == string(virDomainGetName(domPtr))) {
				currMap = testMap;
				break;
			}
			if (*(++next))
				;
		}
		virDomainStatsRecordListFree(records);
		records = next = NULL;
	}
	return currMap;
}

void VM::shutdown() { virDomainShutdown(domPtr); }

string VM::getName() {
	string name = virDomainGetName(domPtr);
	return name;
}

double VM::convertStatMapToUtil(unordered_map<string, string> &map) {
	size_t vcpu_current = 0, vcpu_maximum = 0;
	auto n_curr = map.find("vcpu.current");
	auto n_max = map.find("vcpu.maximum");
	if (n_curr != map.end() and n_max != map.end()) {
		vcpu_current = atol(n_curr->second.c_str());
		vcpu_maximum = atol(n_max->second.c_str());
	} else {
		cerr << "Manager::convertStatMapToUtil: invalid stat map passed"
			 << endl;
		return 0;
	}
	string cpu_name;
	unordered_map<string, string>::iterator iter;
	double cpu_util = 0;

	for (size_t i = 0; i < vcpu_maximum; i++) {
		cpu_name = "vcpu." + to_string(i);
		iter = map.find(cpu_name + ".state");
		if (iter != map.end() and iter->second == "1") {
			iter = map.find(cpu_name + ".time");
			if (iter != map.end()) cpu_util += atol(iter->second.c_str());
		}
	}
	return cpu_util / vcpu_current;
}

void VM::printUtil(virConnectPtr &conn) {
	string domain_name = getName();
	auto prev_map = getStatsforDomain(conn);
	double time_prev = convertStatMapToUtil(prev_map);
	this_thread::sleep_for(chrono::milliseconds(1000));
	auto curr_map = getStatsforDomain(conn);
	double time_curr = convertStatMapToUtil(curr_map);
	double avg_util = 100 * (time_curr - time_prev) / (1000 * 1000 * 1000);
	avg_util = max(0.0, min(100.0, avg_util));
	cout << domain_name << " util. = " << avg_util << "%" << endl;
}

// void VM::printUtil(virConnectPtr &conn) {
// 	string domain_name = getName();
// 	while (true) {
// 		auto prev_map = getStatsforDomain(conn);
// 		double time_prev = convertStatMapToUtil(prev_map);
// 		this_thread::sleep_for(chrono::milliseconds(1000));
// 		auto curr_map = getStatsforDomain(conn);
// 		double time_curr = convertStatMapToUtil(curr_map);
// 		double avg_util = 100 * (time_curr - time_prev) / (1000 * 1000 * 1000);
// 		avg_util = max(0.0, min(100.0, avg_util));
// 		cout << domain_name << " util. = " << avg_util << "%" << endl;
// 	}
// }