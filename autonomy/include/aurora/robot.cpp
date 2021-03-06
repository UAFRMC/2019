
const char *state_to_string(robot_state_t state)
{
	if (state<0 || state>=state_last) return "unknown_invalid";
	const static char *table[state_last+1]={
	"STOP", ///< EMERGENCY STOP (no motion)
	"drive", ///< normal manual driving
	"backend_driver", ///< drive from backend UI

	"autonomy",
	"setup_raise",
	"setup_extend", // deploy the mining head, must be done before driving
	"setup_lower",
	"find_camera",
	"scan_obstacles",

	"drive_to_mine", ///< autonomous: drive to mining area

	/* Semiauto mine mode entry point: */
	"mine_lower", ///< mining mode: lowering head", driving forward
	"mine_stall", ///< mining mode: raising head (after stall)
	"mine", // actually mine
	"mine_raise", ///< existing mining mode: raise bucket

	"drive_to_dump", ///< drive back to bin
	"dump_align", ///< get lined up

	/* Semiauto dump mode entry points: */
	"dump_contact", ///< final dock-and-dump mode: drive to contact bin
	"dump_raise", ///< raising bucket
	"dump_pull", ///< roll forward
	"dump_rattle", ///< rattle mode to empty bucket
	"dump_push", ///< push hook back

	/* Stow mode (shutdown sequence) */
	"stow", // begin stowing: raise bucket
	"stow_clean", // clean out bucket
	"stowed", // finished stowing (wait forever)

	"last"
	};

	return table[state];
}


