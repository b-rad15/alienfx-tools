#include "MonHelper.h"

// debug print
#ifdef _DEBUG
#define DebugPrint(_x_) OutputDebugString(string(_x_).c_str());
#else
#define DebugPrint(_x_)
#endif

void CMonProc(LPVOID);

extern ConfigFan* fan_conf;

MonHelper::MonHelper() {
	if ((acpi = new AlienFan_SDK::Control())->Probe()) {
		fan_conf->lastSelectedSensor = acpi->sensors.front().sid;
		size_t fansize = acpi->fans.size();
		senBoosts.resize(fansize);
		fanRpm.resize(fansize);
		boostRaw.resize(fansize);
		boostSets.resize(fansize);
		lastBoost.resize(fansize);
		fanSleep.resize(fansize);
		Start();
	}
}

MonHelper::~MonHelper() {
	Stop();
	delete acpi;
}

void MonHelper::Start() {
	// start thread...
	if (!monThread) {
		if (oldPower < 0)
			oldPower = acpi->GetGMode() ? (short)acpi->powers.size() : acpi->GetPower();
		SetCurrentMode(fan_conf->lastProf->gmode ? acpi->powers.size() : fan_conf->lastProf->powerStage);
		monThread = new ThreadHelper(CMonProc, this, 750, THREAD_PRIORITY_BELOW_NORMAL);
#ifdef _DEBUG
		OutputDebugString("Mon thread start.\n");
#endif
	}
}

void MonHelper::Stop() {
	if (monThread) {
		delete monThread;
		monThread = NULL;
		SetCurrentMode(oldPower);
#ifdef _DEBUG
		OutputDebugString("Mon thread stop.\n");
#endif
	}
}

void MonHelper::SetCurrentMode(size_t newMode) {
	if (newMode < acpi->powers.size()) {
		if (acpi->GetGMode()) {
			acpi->SetGMode(0);
		}
		acpi->SetPower(acpi->powers[newMode]);
	}
	else {
		if (!acpi->GetGMode()) {
			if (acpi->GetSystemID() == 2933 || acpi->GetSystemID() == 3200) // m15R5 && G5 5510 fix
				acpi->SetPower(0xa0);
			acpi->SetGMode(1);
		}
	}
}

void MonHelper::SetCurrentGmode(bool newMode) {
	fan_conf->lastProf->gmode = acpi->isGmode ? newMode : 0;
	SetCurrentMode(newMode ? acpi->powers.size() : fan_conf->lastProf->powerStage);
}

byte MonHelper::GetFanPercent(byte fanID)
{
	if (!fan_conf->boosts[fanID].maxRPM)
		fan_conf->boosts[fanID].maxRPM = acpi->GetMaxRPM(fanID);
	return (fanRpm[fanID] * 100) / fan_conf->boosts[fanID].maxRPM;
}

void CMonProc(LPVOID param) {
	MonHelper* src = (MonHelper*) param;
	AlienFan_SDK::Control* acpi = src->acpi;
	auto prof = fan_conf->lastProf;
	// Stop if no active profile - just for sure
	if (!prof)
		return;
	bool modified = false;
	// let's check power...
	if (src->inControl && !prof->gmode && acpi->GetPower() != prof->powerStage)
		acpi->SetPower(acpi->powers[prof->powerStage]);
	// update values:
	// temps..
	for (int i = 0; i < acpi->sensors.size(); i++) {
		int temp = acpi->GetTempValue(i);
		// find or create maps...
		if (temp != src->senValues[acpi->sensors[i].sid]) {
			modified = true;
			src->senValues[acpi->sensors[i].sid] = temp;
			src->maxTemps[acpi->sensors[i].sid] = max(src->maxTemps[acpi->sensors[i].sid], temp);
		}
	}

	// fans...
	for (int i = 0; i < acpi->fans.size(); i++) {
		src->boostSets[i] = 0;
		src->boostRaw[i] = acpi->GetFanBoost(i);
		src->fanRpm[i] = acpi->GetFanRPM(i);
		if (src->inControl && modified && !prof->powerStage && !prof->gmode) {
			auto cIter = prof->fanControls.find(i);
			if (cIter != prof->fanControls.end()) {
				// Set boost
				short fnum = cIter->first;
				for (auto fIter = cIter->second.begin(); fIter != cIter->second.end(); fIter++) {
					sen_block* cur = &fIter->second;
					if (cur->active && cur->points.size()) {
						int cBoost = cur->points.back().boost;
						for (auto k = cur->points.begin() + 1; k != cur->points.end(); k++)
							if (src->senValues[fIter->first] <= k->temp) {
								if (k->temp != (k - 1)->temp)
									cBoost = ((k - 1)->boost + ((k->boost - (k - 1)->boost) * (src->senValues[fIter->first] - (k - 1)->temp))
										/ (k->temp - (k - 1)->temp));
								else
									cBoost = k->boost;
								break;
							}
						if (cBoost > src->boostSets[fnum]) {
							src->boostSets[fnum] = cBoost;
							src->lastBoost[fnum] = fIter->first;
						}
						src->senBoosts[fnum][fIter->first] = cBoost;
					}
				}
				int rawBoost = (int)round((fan_conf->GetFanScale((byte)fnum) * src->boostSets[fnum]) / 100.0);
				if (!src->fanSleep[fnum] || rawBoost <= 100) {
					byte boostOld = src->boostRaw[fnum];
					src->fanSleep[fnum] = 0;
					// Check overboost tricks...
					if (boostOld < 90 && rawBoost > 100) {
						acpi->SetFanBoost(fnum, 100);
						src->fanSleep[fnum] = ((100 - boostOld) >> 3) + 2;
						DebugPrint("Overboost started, fan " + to_string(fnum) + " locked for " + to_string(src->fanSleep[fnum]) + " tacts(old "
							+ to_string(boostOld) + ", new " + to_string(rawBoost) + ")!\n");
					}
					else
						if (rawBoost != boostOld) {
							if (boostOld > rawBoost)
								rawBoost += 15 * ((boostOld - rawBoost) >> 4);
							// fan RPM stuck patch v2
							//if (acpi->GetSystemID() == 3200 && src->boostRaw[i] > 50) {
							//	int pct = acpi->GetFanPercent(i) << 3;
							//	if (pct > 105 || pct < src->boostRaw[i]) {
							//		acpi->SetGMode(true);
							//		Sleep(300);
							//		acpi->SetGMode(false);
							//		acpi->SetPower((byte)fan_conf->lastProf->powerStage);
							//		DebugPrint("RPM fix engaged!\n");
							//	}
							//}

							acpi->SetFanBoost(fnum, rawBoost);

							//DebugPrint(("Boost for fan#" + to_string(i) + " changed from " + to_string(src->boostRaw[i])
							//	+ " to " + to_string(src->boostSets[i]) + "\n").c_str());
						}
				}
				else
					src->fanSleep[fnum]--;
			}
		}
	}
}
