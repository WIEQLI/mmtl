/*
FACILITY:           NMMTL
MODULE DESCRIPTION: Contains the global function nmmtl_parse_graphic, and
                    the static functions which it calls:
AUTHOR(S):          David Endry
CREATION DATE:      1-1-86
COPYRIGHT:          Copyright (C) 1986-92 by Mayo Foundation. All rights reserved.
*/


/*
 *******************************************************************
 **  INCLUDE FILES
 *******************************************************************
 */

#include "nmmtl.h"
#include "electro_prototype.h"

#include <string.h>


/*
 *******************************************************************
 **  FUNCTION DEFINITIONS
 *******************************************************************
 */

//---------------------------------------------------------------------
// parseVal: parse the line from the file to get the portion containing
//           the value.
//---------------------------------------------------------------------
void parseVal (char *line, int flg, char *tmp) {
  if (flg)
    sscanf (line, "%*s %s", tmp);
  else
    sscanf (line, "%*s %*s %s", tmp);

  // Remove (optional) surrounding quotes from value
  if ( tmp[0] == '\"' )
    tmp[0] = ' ';
  if ( tmp[strlen(tmp)-1] == '\"' )
    tmp[strlen(tmp)-1] = '\0';
  else if ( tmp[strlen(tmp)-2] == '\"' )
    tmp[strlen(tmp)-2] = '\0';
}


/*
 FUNCTION NAME:  nmmtl_parse_graphic()

 FUNCTIONAL DESCRIPTION:

 generate a data structure of subsections and dielectric constants
 around signals, grounds and dielectric interfaces represented by a 2D
 GPGE graphic cross section diagram of an electromagnetic system. These
 structures are then used to build a mesh of boundary elements in order
 to allow calculation of static EM parameters.  Separate linked lists
 are used for signal and ground wires but both are called contours (use
 contour data struct). Signal and ground wires may be gpge rectangles,
 circles or polygon primitives. Signal wires are always yellow, ground
 wires dark blue, ground planes must use the icon ground_plane (this is
 a dark blue rectangle). Only the number of ground planes are
 important. If 2 ground planes exist, points are generated along the
 top's bottom (two consecutive points make a segment). Dielectric
 layers are represented by hollow (not filled) gpge white or green
 rectangles. A linked list of dielectric layers is created, points
 along dielectric interfaces (line where two adjacent dielectric layers
 touch) are output.  Primitive attributes are used to define
 coordinates of signals, ground wires and dielectric layers making the
 diagram NOT_SCALE.  The old MMTL program implemented two types of
 drawings, a to-scale and a not-to-scale.  This program only implements
 a not-to-scale.  The attributes must be there.

 An attribute on the drawing indicates the units used, either meters,
 microns, mils, or inches.  Mils is assumed if this attribute is
 absent.

 If the user desires to use a transmission line x-section in a network
 simulation, an icon must be created.  If the icon exists, this program
 will match the signal names with the icon pin names and generate
 netlist for the icon.

 Some features have been ommitted.  Some of the code remains, blocked
 by ifdefs.  These are CHECK_SCALE and SORT_DIELECTRICS.  Some of the
 comments from omitted features:

 In the dielectric list, the rightmost link is the bottom layer
 (d_bottom) the left most is the top layer.
 .. otherwise gpge coordinates are used for the
 TO_SCALE primitives. There may be no mixed scale diagrams.


 FORMAL PARAMETERS:

 INPUTS:

 input file: node.graphic

 OUTPUTS:

 cntr_seg : number of contour segments (defaults to 6)
 pln_seg  : number of plane segments (defaults to 15)
 coupling : coupling length of transmission line in meters
 risetime : risetime at which to do the analysis in seconds
 conductivity : conductivity in mhos/meter
 half_minimum_dimension : half the smallest side or radius
 gnd_planes : the number of ground planes : 1 or 2
 top_ground_plane_thickness : in meters
 bottom_ground_plane_thickness : in meters
 dielectrics : pointer to linked list of dielectric layers
 signals  : pointer to the head of the signal linked list
 groundwires  : pointer to the head of the ground wire linked list
 num_signals : number of signal conductors
 num_grounds : number of ground conductors (including upper plane)
 units : the user-specified or default units for measurement

 FUNCTIONS CALLED:
 in_string, header_c, mcms_access, mcms_deaccess,
 get_error, filescan, get_node, get_design_dir


 RETURN VALUE:

 SUCCESS, FAIL

 */
int nmmtl_parse_xsctn(char *filename,
      int *cntr_seg,
      int *pln_seg,
      double *coupling,
      double *risetime,
      double *conductivity, //FIXME: unused argument
      double *half_minimum_dimension,
      int *gnd_planes,
      double *top_ground_plane_thickness,
      double *bottom_ground_plane_thickness,
      struct dielectric **dielectrics,
      struct contour **signals,
      struct contour **groundwires,
      int *num_signals,
      int *num_grounds,
      int *units) {
  char meters[] = "meters";
  char seconds[] = "seconds";
  char siemensPmeter[] = "siemens/meter";

  char line[GPGE_MAX];    /* input GPGE graphic file lines */
  char msg[80];     /* custom string error messages */
  struct dielectric *d_temp;  /* temps for manipulating lists */
                                /* to create, update lists */
  struct contour *c_temp,*c_prev;
  int status; /* function return status */
  NETLIST_REC *netlist;
  double offset = 1.0e20; /* find offset of top of bottom ground plane */
                                /* initialize to some big number */

  double highest_dielectric = -1.0e20; /* the level of the top of the highest */
                                      /* dielectric, initialized to some very*/
                                      /* unlikely number. */
  double minimum_dimension = DBL_MAX;

  int upper_ground_planes = 0;  /* keep count of drawn ground planes */
  int lower_ground_planes = 0;
  double ground_x_min = DBL_MAX, ground_x_max = DBL_MIN;

  char fullfilespec[1024];
  double totWidth = 0;

  /* initialize the parameters set by icon attributes */
  *cntr_seg = 0;
  *pln_seg  = 0;
  *coupling = 0.;
  *risetime = 0.;

  //
  // Establish a default CONDUCTIVITY if the user specified no value.
  //
  *conductivity = DEFAULT_CONDUCTIVITY;
  *gnd_planes = 0;
  *top_ground_plane_thickness = 0.;
  *bottom_ground_plane_thickness = 0.;
  *units = UNITS_NO_UNITS;

  netlist = (NETLIST_REC *)calloc(1,sizeof(NETLIST_REC));
  netlist->exists = FALSE;

  // now try to open the file
  sprintf(fullfilespec, "%s.xsctn", filename);

  FILE *inpf;

  // Loss-tangent not used for the calculations.
  printf ("Warning: lossTangent not used in this simulation!\n");

  if (!(inpf = fopen(fullfilespec, "r"))) {
    printf ("Error: cannot open the cross-section file %s\n", fullfilespec);
    return (FAIL);
  }

  double dbl;
  char tmp[100];
  char defaultUnits[20];
  double arg1;
  char arg2[30];
  int tmp_cntr_seg;
  int tmp_pln_seg;

  while (true) {
    if (fgets(line, GPGE_MAX, inpf) == NULL) {
      printf ("*** EOF incountered -- incomplete input file %s\n", fullfilespec);
      return FAIL;
    }
    // skip comment
    if (line[0] == '#')
      continue;

    // skip for reasons of sanity, line too short to carry relevant information
    if (strlen(line) < 3)
      continue;

    // skip specification of required packages
    if (strstr(line, "ackage") != NULL)
      continue;

    // beginning of geometry section -- exit loop
    if ((! strncmp(line, "Gro", 3))
    ||  (! strncmp(line, "Die", 3))
    ||  (! strncmp(line, "Rec", 3))
    ||  (! strncmp(line, "Cir", 3))
    ||  (! strncmp(line, "Tra", 3)))
      break;

    //cannot see the use for the following lines.. commenting them for the time being
    //// skip indented "set ..." lines
    //if (strncmp(line, "set", 3)) {
      //continue;
    //}

    if (strstr(line, "couplingLen") != NULL) {
      parseVal(line, 0, tmp);
      if ((sscanf (tmp, "%lf%s", &arg1, arg2)) < 2)
        strcat(tmp, "meters");
      conversion(tmp, meters, dbl);
      *coupling = dbl;
      printf ("Input CouplingLength = %lf\n", dbl);
    }
    if (strstr(line, "riseTime") != NULL) {
      parseVal (line, 0, tmp);
      if ((sscanf(tmp, "%lf%s", &arg1, arg2)) < 2) {
        strcat(tmp, "ps");
        conversion(tmp, seconds, dbl);
        *risetime = dbl;
      } else {
        conversion (tmp, seconds, dbl);
        *risetime = dbl;
      }
      printf("Input RiseTime = %lf\n", *risetime);
    }
    if (strstr (line, "defaultLeng") != NULL) {
      sscanf(line, "%*s %*s %*c%s", tmp);
      tmp[strlen(tmp)-1] = '\0';
      strcpy(defaultUnits, tmp);
      printf("Input Default Units: %s\n", defaultUnits);
    }

    if (strstr(line, "CSEG") != NULL) {
      sscanf(line, "%*s %*s %d", &tmp_cntr_seg);
    }
    if (strstr(line, "DSEG") != NULL) {
      sscanf(line, "%*s %*s %d", &tmp_pln_seg);
    }
  }

  // assign segment counters to corresponding return values
  if (*cntr_seg < 1)
    *cntr_seg = tmp_cntr_seg;

  if (*pln_seg < 1)
    *pln_seg = tmp_pln_seg;

  // Establish a default RISETIME if riseTime is set to zero.
  if (*risetime == 0) {
    *risetime = DEFAULT_RISETIME * 1.0e-12;
    printf ("Assign a default value of %g to risetime\n", *risetime);
  }

  // Establish a default COUPLING if the coupling-length is set to zero.
  // Note here that units input will default to units of mils when the user
  // specifies none.
  if (*coupling == 0) {
    /* make the assignment in terms of meters, first, using default in inches*/
    *coupling = DEFAULT_COUPLING;
    *coupling *= INCHES_TO_METERS;
    /* warn about using default values in user's selected units */
    sprintf(msg, "WARN: Default=%g mils used\n", (float)((*coupling) / MILS_TO_METERS));
    printf ("%s\n", msg);
  } else {
    printf ("CouplingLength = %g\n", *coupling);
  }

  /* assign in user's units */
  *top_ground_plane_thickness = DEFAULT_GND_THICK / MILS_TO_METERS;
  *bottom_ground_plane_thickness = DEFAULT_GND_THICK / MILS_TO_METERS;

  double thickness;
  double yCoord = 0.0;

  while (true) {
    if (strstr (line, "oundPlane") != NULL) {
      if (++(*gnd_planes) > 2) {
        /* count ground planes */
        printf ("* Warning: There are %d groundplanes in the design...reset to 2\n",
          *gnd_planes);
        printf ("***************************************************************\n");
        *gnd_planes = 2;  /* keep it at 2 */
      }
    //    yCoord += DEFAULT_GND_THICK;
    }
    //--------------------------------------------------------------------------------
    // Is this a dielectric definition?
    //--------------------------------------------------------------------------------
    else if (strstr (line, "lectricLayer") != NULL) {
      if (*gnd_planes == 0) {
        printf ("* ERROR: There must be a bottom ground plane!\n");
        return (FAIL);
      }
      //-----------------------------------------------------
      // Create the dielectric structure.
      // d_temp.x0  - left x coordinate of the rectangle
      // d_temp.y0  - lower y coordinate of the rectangle
      // d_temp.x1  - right x coordinate of the rectangle
      // d_temp.y1  - upper y coordinate of the rectangle
      //------------------------------------------------
      d_temp = (struct dielectric *)malloc(sizeof(struct dielectric));
      d_temp->next = NULL;
      d_temp->constant = 1.0;   // Default to 1.0 if no -permittivity attribute in the file..
      d_temp->tangent = 0.0;    // Default to 0 since this is not used in calculations.
      d_temp->x0 = d_temp->x1 = 0;
      //-----------------------------------------------------
      // Process this dielectric until a line is read without the "\" character.
      //-----------------------------------------------------
      while (true) {
        //-----------------------------------------------
        // Thickness of the dielectric
        //-----------------------------------------------
        if (strstr (line, "-thickness") != NULL) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, thickness);
          d_temp->y0 = yCoord;
          yCoord += thickness;
          d_temp->y1 = yCoord;

          /* insert dielectric onto plain list */
          d_temp->next = *dielectrics;
          *dielectrics = d_temp;

          /* keep track of the highest dielectric - to find ground plane */
          if(d_temp->y1 > highest_dielectric) highest_dielectric = d_temp->y1;

          /* keep track of the lowest dielectric - to offset to y=0 */
          if(d_temp->y0 < offset) offset = d_temp->y0;

          /* keep track of smallest dimension */
          if(thickness < minimum_dimension)
            minimum_dimension = thickness;
        }

        //-----------------------------------------------
        // Thickness of the dielectric
        //-----------------------------------------------
        if ( strstr (line, "-permittivity") != NULL )
          sscanf (line, "%*s %lf", &d_temp->constant);

        //-----------------------------------------------
        // End of this dielectric definition
        //-----------------------------------------------
        if ( (strrchr (line, '\\')) == 0 ) {
          break;
        }

        if ( fgets (line, GPGE_MAX, inpf) == NULL ) {
          printf ("*** EOF incountered -- imcomplete input file %s\n", fullfilespec);
          return 0;
        }
      }
    }

    //-----------------------------------------------------
    // Is this a rectangle dielectric?
    //-----------------------------------------------------
    else if ( strstr (line, "ctangleDielec") != NULL ) {
      double width, height, xOffset, yOffset, pitch;
      int indx, number;

            //-----------------------------------------------------
      // Create the dielectric structure.
      // d_temp.x0  - left x coordinate of the rectangle
      // d_temp.y0  - lower y coordinate of the rectangle
      // d_temp.x1  - right x coordinate of the rectangle
      // d_temp.y1  - upper y coordinate of the rectangle
      //------------------------------------------------
      d_temp = (struct dielectric *)malloc(sizeof(struct dielectric));
      d_temp->next = NULL;
      d_temp->constant = 1.0;   // Default to 1.0 if no -permittivity attribute in the file..
      d_temp->tangent = 0.0;    // Default to 0 since this is not used in calculations.

      // Set the default number of RectangleDielectric to 1
      number = 1;
      while (true) {
        if ( strstr (line, "-height") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, height);
        }

        //-----------------------------------------------
        // Width of each rectangular conductor
        //-----------------------------------------------
        if ( strstr (line, "-width") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, width);
        }

        //-----------------------------------------------
        // Permittivity of the dielectric
        //-----------------------------------------------
        if ( strstr (line, "-permittivity") != NULL )
          sscanf (line, "%*s %lf", &d_temp->constant);

        //-----------------------------------------------
        // Number of conductors to the set
        //-----------------------------------------------
        if ( strstr (line, "-number") != NULL )
          sscanf (line, "%*s %d", &number);

        //-----------------------------------------------
        // X-offset for placing the first conductor in the set
        //-----------------------------------------------
        if ( strstr (line, "-xOff") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, xOffset);
        }

        //-----------------------------------------------
        // Y-offset for placing the first conductor in the set
        //-----------------------------------------------
        if ( strstr (line, "-yOff") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, yOffset);
        }

        //-----------------------------------------------
        // Spacing for the conductor set
        //-----------------------------------------------
        if ( strstr (line, "-pitch") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, pitch);
        }

        //-----------------------------------------------
        // End of this dielectric definition
        //-----------------------------------------------
        if ( (strrchr (line, '\\')) == 0 ) {
          break;
        }


        if ( fgets (line, GPGE_MAX, inpf) == NULL ) {
          printf ("*** EOF incountered -- imcomplete input file %s\n", fullfilespec);
          return 0;
        }
      }

      double cx = xOffset;
      for ( indx = 0; indx < number; ++indx ) {
        //-----------------------------------------------
        // d_temp.x0  - left x coordinate of the rectangle
        // d_temp.y0  - lower y coordinate of the rectangle
        // d_temp.x1  - right x coordinate of the rectangle
        // d_temp.y1  - upper y coordinate of the rectangle
        //------------------------------------------------
        d_temp->x0 = xOffset;
        d_temp->x1 = d_temp->x0 + width;
        d_temp->y0 = yCoord;
        d_temp->y1 = yCoord + height;

        /* insert dielectric onto plain list */
        d_temp->next = *dielectrics;
        *dielectrics = d_temp;

        /* keep track of the highest dielectric - to find ground plane */
        if(d_temp->y1 > highest_dielectric) highest_dielectric = d_temp->y1;

        /* keep track of the lowest dielectric - to offset to y=0 */
        if(d_temp->y0 < offset) offset = d_temp->y0;

        /* keep track of smallest dimension */
        if(height < minimum_dimension)
          minimum_dimension = height;

        cx += pitch;
      }
    }
    //-----------------------------------------------------
    // Is this a conductor definition?
    //-----------------------------------------------------
    else if ( strstr (line, "Conductors") != NULL ) {
      // initialize all values with "-1" to cause failure on expected use
      double width = -1.;
      double botWidth = -1.;
      double topWidth = -1.;
      double height   = -1.;
      double xOffset  = -1.;
      double yOffset  = -1.;
      double pitch    = -1.;
      double diameter   = -1.;
      double tmp_cond2  = -1.;
      int indx      = -1;
      int number    = -1;
      int primitive = -1;
      int type      = -1;
      char name[500];

      // Name of the conductor
      sscanf (line, "%*s %s", name);

      // Type of conductor
      if ( strstr (line, "RectangleCon") != NULL )
        type = primitive = RECTANGLE;
      else if ( strstr (line, "TrapezoidCon") != NULL )
        {
          primitive = POLYGON;
          type = 'T';
        }
      else if ( strstr (line, "CircleCon") != NULL )
        {
          primitive = CIRCLE;
          type = 'C';
        }
      // Set the default number of conductors to 1
      number = 1;
      while ( 1 ) {
        //-----------------------------------------------
        // Conductivity of conductor set
        //-----------------------------------------------
        if ( strstr (line, "-conductivity") != NULL ) {
          parseVal (line, 1, tmp);
          conversion (tmp, siemensPmeter, tmp_cond2);
        }

        //-----------------------------------------------
        // Width of each rectangular conductor
        //-----------------------------------------------
        if ( strstr (line, "-width") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, width);
        }

        //-----------------------------------------------
        // Top width of each trapezoidal conductor
        //-----------------------------------------------
        if ( strstr (line, "-topWidth") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, topWidth);
        }

        //-----------------------------------------------
        // Bottom width of each trapezoidal conductor
        //-----------------------------------------------
        if ( strstr (line, "-bottomWidth") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, botWidth);
        }

        //-----------------------------------------------
        // Height of each rectangular or trapezoidal conductor
        //-----------------------------------------------
        if ( strstr (line, "-height") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, height);
        }

        //-----------------------------------------------
        // Diameter of each circular conductor
        //-----------------------------------------------
        if ( strstr (line, "-diameter") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, diameter);
        }

        //-----------------------------------------------
        // Number of conductors to the set
        //-----------------------------------------------
        if ( strstr (line, "-number") != NULL )
          sscanf (line, "%*s %d", &number);

        //-----------------------------------------------
        // X-offset for placing the first conductor in the set
        //-----------------------------------------------
        if ( strstr (line, "-xOff") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, xOffset);
        }

        //-----------------------------------------------
        // Y-offset for placing the first conductor in the set
        //-----------------------------------------------
        if ( strstr (line, "-yOff") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, yOffset);
        }

        //-----------------------------------------------
        // Spacing for the conductor set
        //-----------------------------------------------
        if ( strstr (line, "-pitch") != NULL ) {
          parseVal (line, 1, tmp);
          if ( (sscanf (tmp, "%lf%s", &arg1, arg2)) < 2 )
            strcat (tmp, defaultUnits);
          conversion (tmp, meters, pitch);
        }

        //-----------------------------------------------
        // Break is this is the end of the conductor definition
        //-----------------------------------------------
        if ( (strrchr (line, '\\')) == 0 )
    ///       if ( line[strlen(line)-2] != '\\' )
          break;
        if ( fgets (line, GPGE_MAX, inpf) == NULL ) {
          printf ("*** EOF incountered -- imcomplete input file %s\n", fullfilespec);
          return 0;
        }

        if ( strlen(line) < 1 )
          continue;
      }

      double cx = xOffset;
      double cy = yCoord + yOffset;
      // initialize "tw" and "length" values that will most likely lead to
      // obvious failure, if used unexpectedly
      double tw     = -1.;
      double length = -1.;

      //-----------------------------------------------------
      // Caculate the total width of the conductor set.
      // Check for the minimum width of the conductor.
      //-----------------------------------------------------
      switch ( primitive ) {
      case RECTANGLE:
        tw = xOffset + (number - 1) * pitch + width;
        if ( width < minimum_dimension )
          minimum_dimension = width;
        if ( height < minimum_dimension )
          minimum_dimension = height;
        break;
      case POLYGON:
        width = botWidth;
        if ( topWidth > width )
          width = topWidth;
        tw = xOffset + (number - 1) * pitch + width;
        width *= 0.5;
        if(topWidth < minimum_dimension)
          minimum_dimension = topWidth;
        if(botWidth < minimum_dimension)
          minimum_dimension = botWidth;
        break;
      case CIRCLE:
        tw = xOffset + (number - 1) * pitch + diameter;
        if ( diameter < minimum_dimension )
          minimum_dimension = diameter;
        break;
      }
      if ( tw > totWidth ) {
        totWidth = tw;
        printf ("Total width: %g\n", totWidth);
      }

      for ( indx = 0; indx < number; ++indx ) {
        c_temp = (struct contour *)malloc(sizeof(struct contour));
        c_temp->next = NULL;
        c_temp->points = NULL;
        c_temp->name[0] = '\0';
        c_temp->x0 = cx;
        c_temp->y0 = cy;
        switch ( primitive ) {
        case RECTANGLE:
          //-----------------------------------------------
          // c_temp.x0  - left x coordinate of the rectangle
          // c_temp.y0  - lower y coordinate of the rectangle
          // c_temp.x1  - right x coordinate of the rectangle
          // c_temp.y1  - upper y coordinate of the rectangle
          //------------------------------------------------
          c_temp->x1 = cx + width;
          c_temp->y1 = cy + height;
          break;
        case CIRCLE:
          //-----------------------------------------------
          // c_temp.x0  - center x point of the circle
          // c_temp.y0  - center y point of the circle
          // c_temp.x1  - radius of the circle
          //------------------------------------------------
          c_temp->x0 += diameter * 0.5;
          c_temp->y0 += diameter * 0.5;
          c_temp->x1 = diameter * 0.5;
          c_temp->y1 = 0;
          break;
        case POLYGON:
          //-----------------------------------------------
          // c_temp.x0  - lengh of the line segments of the polygon
          // c_temp.y0  - lower y coordinate of the polygon
          // c_temp.x1  - 0
          // c_temp.y1  - upper y coordinate of the rectangle
          //------------------------------------------------
          struct polypoints *pt, *ppt;

          c_temp->x0 = 0.0;
          c_temp->x1 = 0.0;
          c_temp->y1 = cy + height;

          //------------------------------------------------
          // Create the line-segments that define the polygon.
          //------------------------------------------------
          c_temp->points = pt = (struct polypoints *)malloc(sizeof(struct polypoints));
          pt->x = cx + width - botWidth * 0.5;
          pt->y = cy;

          pt->next = ppt = (struct polypoints *)malloc(sizeof(struct polypoints));
          ppt->x = cx + width - topWidth * 0.5;
          ppt->y = cy + height;

          length = sqrt(pow(fabs(ppt->x - pt->x), 2.0) +
            pow(fabs(ppt->y - pt->y), 2.0));

          /* add segment length to perimeter */
          c_temp->x0 += length;

          /* keep track of smallest dimension */
          if(length < minimum_dimension)
            minimum_dimension = length;

          ppt->next = pt = (struct polypoints *)malloc(sizeof(struct polypoints));
          pt->x = cx + width + topWidth * 0.5;
          pt->y = cy + height;

          length = sqrt(pow(fabs(ppt->x - pt->x), 2.0) +
            pow(fabs(ppt->y - pt->y), 2.0));

          /* add segment length to perimeter */
          c_temp->x0 += length;

          /* keep track of smallest dimension */
          if(length < minimum_dimension)
            minimum_dimension = length;

          pt->next = ppt = (struct polypoints *)malloc(sizeof(struct polypoints));
          ppt->x = cx + width + botWidth * 0.5;
          ppt->y = cy;

          length = sqrt(pow(fabs(ppt->x - pt->x), 2.0) +
            pow(fabs(ppt->y - pt->y), 2.0));

          /* add segment length to perimeter */
          c_temp->x0 += length;

          /* keep track of smallest dimension */
          if(length < minimum_dimension)
            minimum_dimension = length;

          ppt->next = pt = (struct polypoints *)malloc(sizeof(struct polypoints));
          pt->x = cx + width - botWidth * 0.5;
          pt->y = cy;

          length = sqrt(pow(fabs(ppt->x - pt->x), 2.0) +
            pow(fabs(ppt->y - pt->y), 2.0));

          /* add segment length to perimeter */
          c_temp->x0 += length;

          /* keep track of smallest dimension */
          if(length < minimum_dimension)
            minimum_dimension = length;

          pt->next = NULL;
          break;
        default:
          printf ("***** Should never get here!!! Invalid primitive type!! *****\n");
        }

        // RECTANGLE, POLYGON, or CIRCLE
        c_temp->primitive = primitive;

        //----------------------------------------------------
        // Check if this is a conductor that should be defined as a ground wire.
        //----------------------------------------------------
        if (strncmp(name, "gr", 2)) {
          c_temp->conductivity = tmp_cond2;
          c_temp->next = *signals;
          *signals = c_temp;
          sprintf(c_temp->name, "%s%c%d", name, type, *num_signals);
          (*num_signals)++;
          printf("Conductivity %s = %g siemens/meter\n",
                 c_temp->name,
                 c_temp->conductivity);
        } else {
          c_temp->next = *groundwires;
          c_temp->conductivity = 0.;
          c_temp->name[0] = '\0';
          *groundwires = c_temp;
          (*num_grounds)++;
        }
        cx += pitch;
      }
    }
    if ( fgets (line, GPGE_MAX, inpf) == NULL )
      break;

    if ( strlen(line) < 1 )
      continue;
  }

  d_temp = *dielectrics;
  while ( d_temp != NULL ) {
    if ( d_temp->x0 == 0 && d_temp->x1 == 0 ) {
      d_temp->x0 = -totWidth;
      d_temp->x1 = d_temp->x0 + 3.0 * totWidth;
    }
    d_temp = d_temp->next;
  }

  /* Offset all y dimensions to make top of bottom ground plane at */
  /* y=0.0 */

  if (offset != 0.0) {
    status = nmmtl_set_offset(offset, *dielectrics, *signals, *groundwires);
    if (status != SUCCESS) {
      printf("ERROR in nmmtl_set_offset: Cannot set offset\n");
      exit(FAIL);
    }
  }
  highest_dielectric -= offset;

  /* now, check to see if ground planes were really specified by */
  /* drawing rectanges.  If so, the bottom ground plane will be */
  /* now at y=0, since the offset operation was completed. */

  /* ordinarily there should be one of each, but the following code */
  /* is extra robust, just in case there are more. */

  if (*gnd_planes == 0 && *groundwires != NULL) {
    /* loop and take off all elements on groundwire list which */
    /* would qualify as lower ground planes.  If there is more */
    /* than one, so warn the user. */

    c_prev = NULL;
    c_temp = *groundwires;
    while(c_temp != NULL) {
      if(c_temp->primitive == 'R') {
        if(c_temp->y1 == 0.0) {
          lower_ground_planes++;
          if(lower_ground_planes > 1) {
            printf ("**** Too many lower groundplanes...reset to 1\n");
          }
          *gnd_planes = 1;
          (*num_grounds)--;

          /* save fullest extent of x dimensions of ground plane */
          if(ground_x_min > c_temp->x0) ground_x_min = c_temp->x0;
          if(ground_x_max < c_temp->x1) ground_x_max = c_temp->x1;

          /* record ground plane thickness */
          *bottom_ground_plane_thickness = c_temp->y1 - c_temp->y0;

          if(c_prev == NULL) {
            /* remove first item from the list */
            *groundwires = c_temp->next;
            free(c_temp);
            c_temp = *groundwires;
          } else {
            /* remove from list */
            c_prev->next = c_temp->next;
            free(c_temp);
            c_temp = c_prev->next;
          }
        } else {
          c_prev = c_temp; c_temp = c_temp->next;
        }
      } else {
        c_prev = c_temp; c_temp = c_temp->next;
      }
    }
  }


  /* Now try to find an upper ground plane - its x dimensions must */
  /* match exactly */

  if(*gnd_planes == 1 && *groundwires != NULL) {
    /* loop and take off all elements on groundwire list which */
    /* would qualify as an upper ground planes.  If there is more */
    /* than one, so warn the user. */

    c_prev = NULL;
    c_temp = *groundwires;
    while(c_temp != NULL) {
      if(c_temp->primitive == 'R') {
        if (c_temp->y0 == highest_dielectric &&
        ground_x_min == c_temp->x0 && ground_x_max == c_temp->x1 ) {
          (*num_grounds)--;

          upper_ground_planes++;
          if(upper_ground_planes > 1)
          {
            printf ("**** Too many upper groundplanes...reset to 2\n");
          }
          *gnd_planes = 2;

          /* record ground plane thickness */
          *top_ground_plane_thickness = c_temp->y1 - c_temp->y0;

          /* remove from the list of groundwires */
          if(c_prev == NULL) {
            /* remove first item from the list */
            *groundwires = c_temp->next;
            free(c_temp);
            c_temp = *groundwires;
          } else {
            /* remove from list */
            c_prev->next = c_temp->next;
            free(c_temp);
            c_temp = c_prev->next;
          }
        } else {
          c_prev = c_temp; c_temp = c_temp->next;
        }
      } else {
        c_prev = c_temp; c_temp = c_temp->next;
      }
    }
  }

  /* warning to the user if bottom ground plane is missing - it
     will go on and assume one exists, below the lowest dielectric layer */
  if(*gnd_planes < 1) {
    printf ("* Warning: There isn't a groundplane\n");
  }

  *half_minimum_dimension = .5 * minimum_dimension;

  /* all done, return status of close_files */
  fclose (inpf);
  return (SUCCESS);
}
